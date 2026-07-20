/**
 * @file panel_offline_job.c
 * @brief Reads selected SD G-code and sends it to RX using SLE Job packets.
 */
#include "panel_offline_job.h"
#include "panel_file_manager.h"
#include "panel_job_proto.h"
#include "panel_model.h"
#include "panel_rx_commands.h"
#include "panel_transport_sle.h"
#include "task_manager.h"

#include "crc16.h"
#include "packet.h"
#include "protocol.h"
#include "securec.h"
#include "sle_errcode.h"
#include "soc_osal.h"
#include "systick.h"

#include <stdio.h>
#include <string.h>

#define PANEL_OFFLINE_TASK_STACK_SIZE 0x3000
#define PANEL_OFFLINE_TASK_PRIORITY   5
#define PANEL_OFFLINE_JOB_ID          2U
#define PANEL_OFFLINE_CONNECT_WAIT_MS 8000U
#define PANEL_OFFLINE_ACK_TIMEOUT_MS  2000U
#define PANEL_OFFLINE_RETRY_MAX       8U
#define PANEL_OFFLINE_STATUS_MS       1000U
#define PANEL_OFFLINE_EXEC_TIMEOUT_MS 600000U
#define PANEL_OFFLINE_CHUNK_MAX       300U
#define PANEL_OFFLINE_DATA_WINDOW_PACKETS 3U
#define PANEL_OFFLINE_DATA_WINDOW_POLL_MS 5U
#define PANEL_OFFLINE_DATA_DRAIN_TIMEOUT_MS 8000U
#define PANEL_OFFLINE_USE_FULL_FILE_CRC 0
#define PANEL_OFFLINE_READAHEAD_BYTES 8192U
#define PANEL_OFFLINE_PROGRESS_PACKETS 4U
#define PANEL_OFFLINE_PREROLL_REQUEST_BYTES 4096U
#define PANEL_OFFLINE_RX_CACHE_SIZE 102400U
#define PANEL_OFFLINE_TIMING_FIRST_PACKETS 0U
#define PANEL_OFFLINE_TIMING_EVERY_PACKETS 0U
#define PANEL_OFFLINE_TIMING_SLOW_MS 500U
#define PANEL_OFFLINE_READ_LOG_BYTES 32768U
#define PANEL_OFFLINE_READ_SLOW_MS 100U

_Static_assert(sizeof(job_data_payload_t) + PANEL_OFFLINE_CHUNK_MAX <= SLE_JOB_PACKET_MAX_PAYLOAD,
               "PANEL_OFFLINE_CHUNK_MAX too large for SLE payload");

typedef struct {
    uint32_t effective_offset;
    size_t prefetched_len;
    bool prefetched_eof;
} offline_preroll_plan_t;

typedef enum {
    PANEL_OFFLINE_FLOW_OK = 0,
    PANEL_OFFLINE_FLOW_FAIL,
    PANEL_OFFLINE_FLOW_ABORTED,
} panel_offline_flow_t;

static osal_semaphore g_ack_sem;
static volatile bool g_ack_sem_ready;
static osal_semaphore g_start_sem;
static volatile bool g_start_sem_ready;
static volatile bool g_worker_ready;
static volatile bool g_wait_active;
static volatile bool g_wait_got_ack;
static volatile uint16_t g_wait_ack_seq;
static volatile uint8_t g_wait_status;
static volatile bool g_busy;
static volatile bool g_start_requested;
static volatile uint32_t g_data_ack_offset;
static volatile uint16_t g_data_ack_seq;
static volatile uint8_t g_data_ack_status = JOB_STATUS_OK;
static volatile uint32_t g_data_ack_credit;
static volatile bool g_data_error;
static uint8_t g_pending_index;
static uint8_t g_readahead_buf[PANEL_OFFLINE_READAHEAD_BYTES];
static uint32_t g_diag_data_count;

static uint16_t payload_len_for_type(uint8_t type, const void *payload, uint16_t fallback)
{
    if (type == PKT_JOB_DATA && payload != NULL) {
        const job_data_payload_t *p = (const job_data_payload_t *)payload;
        return (uint16_t)(sizeof(job_data_payload_t) + p->data_len);
    }
    return fallback;
}

static bool should_log_packet_timing(uint8_t type, uint32_t data_index, uint32_t total_ms)
{
    if (type != PKT_JOB_DATA) {
        return true;
    }
    return data_index <= PANEL_OFFLINE_TIMING_FIRST_PACKETS ||
           (PANEL_OFFLINE_TIMING_EVERY_PACKETS > 0U &&
            (data_index % PANEL_OFFLINE_TIMING_EVERY_PACKETS) == 0U) ||
           total_ms >= PANEL_OFFLINE_TIMING_SLOW_MS;
}

static uint32_t offline_data_window_bytes(void)
{
    uint32_t packets = PANEL_OFFLINE_DATA_WINDOW_PACKETS;
    if (packets == 0U) {
        packets = 1U;
    }
    return packets * PANEL_OFFLINE_CHUNK_MAX;
}

static void reset_data_ack_state(void)
{
    g_data_ack_offset = 0;
    g_data_ack_seq = 0;
    g_data_ack_status = JOB_STATUS_OK;
    g_data_ack_credit = 0;
    g_data_error = false;
}

static bool handle_data_ack(const ack_payload_t *ack)
{
    if (ack == NULL || ack->ack_type != PKT_JOB_DATA || !g_busy) {
        return false;
    }
    if (g_wait_active && ack->ack_seq == g_wait_ack_seq) {
        return false;
    }

    if (ack->status != JOB_STATUS_OK) {
        g_data_ack_offset = ack->offset;
        g_data_ack_seq = ack->ack_seq;
        g_data_ack_status = ack->status;
        g_data_ack_credit = ack->credit;
        g_data_error = true;
        osal_printk("[PANEL_DATA_NACK] t=%u seq=%u status=%u off=%u credit=%u wait=%u active=%u\r\n",
                    (unsigned int)uapi_systick_get_ms(), ack->ack_seq,
                    (unsigned int)ack->status, (unsigned int)ack->offset,
                    (unsigned int)ack->credit, (unsigned int)g_wait_ack_seq,
                    (unsigned int)g_wait_active);
        return true;
    }

    if (ack->offset < g_data_ack_offset) {
        osal_printk("[PANEL_DATA_OLD_ACK] t=%u seq=%u off=%u current_off=%u credit=%u\r\n",
                    (unsigned int)uapi_systick_get_ms(), ack->ack_seq,
                    (unsigned int)ack->offset, (unsigned int)g_data_ack_offset,
                    (unsigned int)ack->credit);
        return true;
    }

    g_data_ack_offset = ack->offset;
    g_data_ack_seq = ack->ack_seq;
    g_data_ack_status = ack->status;
    g_data_ack_credit = ack->credit;

    if (PANEL_OFFLINE_TIMING_EVERY_PACKETS > 0U &&
        ((uint32_t)ack->ack_seq % PANEL_OFFLINE_TIMING_EVERY_PACKETS) == 0U) {
        osal_printk("[PANEL_DATA_ACK] t=%u seq=%u off=%u credit=%u\r\n",
                    (unsigned int)uapi_systick_get_ms(), ack->ack_seq,
                    (unsigned int)ack->offset, (unsigned int)ack->credit);
    }
    return true;
}

static void rx_response_cb(const uint8_t *data, uint16_t len)
{
    sle_packet_view_t pkt;
    if (!sle_packet_decode(data, len, &pkt)) {
        return;
    }

    if ((pkt.type != PKT_ACK && pkt.type != PKT_NACK) ||
        pkt.len != sizeof(ack_payload_t)) {
        return;
    }

    ack_payload_t ack;
    (void)memcpy_s(&ack, sizeof(ack), pkt.payload, sizeof(ack));
    if (handle_data_ack(&ack)) {
        return;
    }

    if (!g_wait_active || ack.ack_seq != g_wait_ack_seq) {
        return;
    }

    g_wait_status = ack.status;
    g_wait_got_ack = true;
    if (g_ack_sem_ready) {
        osal_sem_up(&g_ack_sem);
    }
}

static errcode_t wait_rx_link(uint32_t timeout_ms)
{
    uint32_t start = (uint32_t)uapi_systick_get_ms();
    while (!panel_transport_sle_rx_is_connected()) {
        if ((uint32_t)((uint32_t)uapi_systick_get_ms() - start) >= timeout_ms) {
            return ERRCODE_SLE_TIMEOUT;
        }
        panel_transport_sle_poll();
        osal_msleep(20);
    }
    return ERRCODE_SUCC;
}

static errcode_t send_packet_wait_ack(uint8_t type, const void *payload, uint16_t payload_len)
{
    uint8_t packet[SLE_JOB_PACKET_MAX_SIZE];
    uint16_t packet_len = 0;
    uint16_t seq = panel_job_proto_next_seq();
    uint16_t actual_payload_len = payload_len_for_type(type, payload, payload_len);
    uint32_t dbg_off = 0;
    uint16_t dbg_dlen = 0;
    uint32_t dbg_data_index = 0;

    if (!sle_packet_encode(type, 0, seq, payload, actual_payload_len,
                           packet, sizeof(packet), &packet_len)) {
        osal_printk("[PANEL_OFFLINE] encode fail type=0x%02x len=%u\r\n",
                    type, (unsigned int)actual_payload_len);
        return ERRCODE_FAIL;
    }

    if (wait_rx_link(PANEL_OFFLINE_CONNECT_WAIT_MS) != ERRCODE_SUCC) {
        osal_printk("[PANEL_OFFLINE] no RX link type=0x%02x\r\n", type);
        return ERRCODE_SLE_TIMEOUT;
    }

    if (type == PKT_JOB_DATA && payload != NULL &&
        actual_payload_len >= sizeof(job_data_payload_t)) {
        const job_data_payload_t *dp = (const job_data_payload_t *)payload;
        dbg_off = dp->offset;
        dbg_dlen = dp->data_len;
        g_diag_data_count++;
        dbg_data_index = g_diag_data_count;
    }

    g_wait_ack_seq = seq;
    g_wait_active = true;

    for (uint32_t retry = 0; retry <= PANEL_OFFLINE_RETRY_MAX; retry++) {
        while (g_ack_sem_ready && osal_sem_down_timeout(&g_ack_sem, 0) == OSAL_SUCCESS) {
        }
        g_wait_status = JOB_STATUS_INTERNAL_ERROR;
        g_wait_got_ack = false;
        uint32_t t_send = (uint32_t)uapi_systick_get_ms();
        errcode_t ret = panel_transport_sle_send_rx_packet(packet, packet_len);
        uint32_t send_ms = (uint32_t)uapi_systick_get_ms() - t_send;
        if (ret == ERRCODE_SLE_SUCCESS &&
            osal_sem_down_timeout(&g_ack_sem, PANEL_OFFLINE_ACK_TIMEOUT_MS) == OSAL_SUCCESS &&
            g_wait_got_ack) {
            uint32_t ack_ms = (uint32_t)uapi_systick_get_ms() - t_send - send_ms;
            uint32_t total_ms = (uint32_t)uapi_systick_get_ms() - t_send;
            if (should_log_packet_timing(type, dbg_data_index, total_ms)) {
                osal_printk("[PANEL_TIMING] type=0x%02x seq=%u data_idx=%u off=%u len=%u "
                            "send_ms=%u ack_ms=%u total_ms=%u retry=%u status=%u\r\n",
                            type, (unsigned int)seq, (unsigned int)dbg_data_index,
                            (unsigned int)dbg_off, (unsigned int)dbg_dlen,
                            (unsigned int)send_ms, (unsigned int)ack_ms,
                            (unsigned int)total_ms, (unsigned int)retry,
                            (unsigned int)g_wait_status);
            }
            g_wait_active = false;
            g_wait_ack_seq = 0;
            if (g_wait_status == JOB_STATUS_OK) {
                return ERRCODE_SUCC;
            }
            osal_printk("[PANEL_OFFLINE] rx nack type=0x%02x seq=%u status=%u off=%u len=%u\r\n",
                        type, (unsigned int)seq, (unsigned int)g_wait_status,
                        (unsigned int)dbg_off, (unsigned int)dbg_dlen);
            return ERRCODE_FAIL;
        }
        osal_printk("[PANEL_OFFLINE] wait ack retry type=0x%02x seq=%u try=%u ret=0x%x "
                    "send_ms=%u st=%u off=%u len=%u\r\n",
                    type, (unsigned int)seq, (unsigned int)retry, ret,
                    (unsigned int)send_ms, (unsigned int)g_wait_status,
                    (unsigned int)dbg_off, (unsigned int)dbg_dlen);
    }

    g_wait_active = false;
    g_wait_ack_seq = 0;
    return ERRCODE_SLE_TIMEOUT;
}

static bool wait_data_window(uint16_t seq, uint32_t data_index,
                             uint32_t offset, uint16_t len)
{
    uint32_t next_offset = offset + len;
    uint32_t window = offline_data_window_bytes();
    uint32_t start = (uint32_t)uapi_systick_get_ms();
    bool logged_wait = false;

    while (!g_data_error) {
        uint32_t ack_offset = g_data_ack_offset;
        uint32_t outstanding = (next_offset > ack_offset) ? (next_offset - ack_offset) : 0U;
        if (outstanding <= window) {
            uint32_t waited = (uint32_t)uapi_systick_get_ms() - start;
            if (waited >= PANEL_OFFLINE_TIMING_SLOW_MS || logged_wait) {
                osal_printk("[PANEL_DATA_WIN_OK] t=%u seq=%u data_idx=%u off=%u len=%u "
                            "next=%u ack_off=%u ack_seq=%u outstanding=%u window=%u "
                            "credit=%u waited=%u\r\n",
                            (unsigned int)uapi_systick_get_ms(), seq,
                            (unsigned int)data_index, (unsigned int)offset,
                            (unsigned int)len, (unsigned int)next_offset,
                            (unsigned int)ack_offset, (unsigned int)g_data_ack_seq,
                            (unsigned int)outstanding, (unsigned int)window,
                            (unsigned int)g_data_ack_credit, (unsigned int)waited);
            }
            return true;
        }

        uint32_t waited = (uint32_t)uapi_systick_get_ms() - start;
        if (!logged_wait && waited >= PANEL_OFFLINE_TIMING_SLOW_MS) {
            logged_wait = true;
            osal_printk("[PANEL_DATA_WIN_WAIT] t=%u seq=%u data_idx=%u off=%u len=%u "
                        "next=%u ack_off=%u ack_seq=%u outstanding=%u window=%u "
                        "credit=%u waited=%u\r\n",
                        (unsigned int)uapi_systick_get_ms(), seq,
                        (unsigned int)data_index, (unsigned int)offset,
                        (unsigned int)len, (unsigned int)next_offset,
                        (unsigned int)ack_offset, (unsigned int)g_data_ack_seq,
                        (unsigned int)outstanding, (unsigned int)window,
                        (unsigned int)g_data_ack_credit, (unsigned int)waited);
        }
        if (waited >= PANEL_OFFLINE_DATA_DRAIN_TIMEOUT_MS) {
            osal_printk("[PANEL_DATA_WIN_TIMEOUT] t=%u seq=%u data_idx=%u off=%u len=%u "
                        "next=%u ack_off=%u ack_seq=%u outstanding=%u window=%u "
                        "status=%u credit=%u waited=%u link=%u\r\n",
                        (unsigned int)uapi_systick_get_ms(), seq,
                        (unsigned int)data_index, (unsigned int)offset,
                        (unsigned int)len, (unsigned int)next_offset,
                        (unsigned int)ack_offset, (unsigned int)g_data_ack_seq,
                        (unsigned int)outstanding, (unsigned int)window,
                        (unsigned int)g_data_ack_status,
                        (unsigned int)g_data_ack_credit,
                        (unsigned int)waited,
                        (unsigned int)panel_transport_sle_rx_is_connected());
            return false;
        }

        panel_transport_sle_poll();
        osal_msleep(PANEL_OFFLINE_DATA_WINDOW_POLL_MS);
    }

    osal_printk("[PANEL_DATA_WIN_ERR] t=%u seq=%u data_idx=%u off=%u len=%u "
                "ack_off=%u ack_seq=%u status=%u credit=%u\r\n",
                (unsigned int)uapi_systick_get_ms(), seq,
                (unsigned int)data_index, (unsigned int)offset,
                (unsigned int)len, (unsigned int)g_data_ack_offset,
                (unsigned int)g_data_ack_seq, (unsigned int)g_data_ack_status,
                (unsigned int)g_data_ack_credit);
    return false;
}

static bool wait_data_drain(uint32_t target_offset)
{
    if (target_offset == 0U) {
        return true;
    }

    uint32_t start = (uint32_t)uapi_systick_get_ms();
    while (!g_data_error && g_data_ack_offset < target_offset) {
        uint32_t elapsed = (uint32_t)uapi_systick_get_ms() - start;
        if (elapsed >= PANEL_OFFLINE_DATA_DRAIN_TIMEOUT_MS) {
            osal_printk("[PANEL_DATA_DRAIN_TIMEOUT] target=%u ack_off=%u ack_seq=%u "
                        "status=%u waited=%u link=%u\r\n",
                        (unsigned int)target_offset,
                        (unsigned int)g_data_ack_offset,
                        (unsigned int)g_data_ack_seq,
                        (unsigned int)g_data_ack_status,
                        (unsigned int)elapsed,
                        (unsigned int)panel_transport_sle_rx_is_connected());
            return false;
        }
        panel_transport_sle_poll();
        osal_msleep(10);
    }

    uint32_t waited = (uint32_t)uapi_systick_get_ms() - start;
    if (g_data_error) {
        osal_printk("[PANEL_DATA_DRAIN_ERR] target=%u ack_off=%u ack_seq=%u status=%u waited=%u\r\n",
                    (unsigned int)target_offset,
                    (unsigned int)g_data_ack_offset,
                    (unsigned int)g_data_ack_seq,
                    (unsigned int)g_data_ack_status,
                    (unsigned int)waited);
        return false;
    }
    if (waited >= PANEL_OFFLINE_TIMING_SLOW_MS) {
        osal_printk("[PANEL_DATA_DRAIN_OK] target=%u ack_off=%u ack_seq=%u waited=%u\r\n",
                    (unsigned int)target_offset,
                    (unsigned int)g_data_ack_offset,
                    (unsigned int)g_data_ack_seq,
                    (unsigned int)waited);
    }
    return true;
}

static errcode_t send_packet_no_ack(uint8_t type, const void *payload, uint16_t payload_len)
{
    uint8_t packet[SLE_JOB_PACKET_MAX_SIZE];
    uint16_t packet_len = 0;
    uint16_t seq = (type == PKT_STATUS_REQ) ?
        panel_job_proto_peek_seq() : panel_job_proto_next_seq();
    if (!sle_packet_encode(type, 0, seq, payload, payload_len,
                           packet, sizeof(packet), &packet_len)) {
        return ERRCODE_FAIL;
    }
    return panel_transport_sle_send_rx_packet(packet, packet_len);
}

static void abort_rx_best_effort(void)
{
    if (panel_transport_sle_rx_is_connected()) {
        (void)send_packet_wait_ack(PKT_JOB_ABORT, NULL, 0);
    }
}

#if PANEL_OFFLINE_USE_FULL_FILE_CRC
static errcode_t compute_file_crc(uint8_t index, uint32_t total_size, uint16_t *out_crc)
{
    uint8_t buf[PANEL_OFFLINE_CHUNK_MAX];
    uint32_t offset = 0;
    uint16_t crc = 0xFFFFU;

    while (offset < total_size) {
        size_t bytes_read = 0;
        bool eof = false;
        if (!panel_file_manager_read_chunk(index, offset, buf, sizeof(buf), &bytes_read, &eof) ||
            bytes_read == 0U) {
            return ERRCODE_FAIL;
        }
        crc = job_crc16_ccitt_update(crc, buf, (uint16_t)bytes_read);
        offset += (uint32_t)bytes_read;
        osal_msleep(1);
        if (eof && offset < total_size) {
            return ERRCODE_FAIL;
        }
    }

    *out_crc = crc;
    return ERRCODE_SUCC;
}
#endif

static bool plan_offline_preroll(uint8_t index, const panel_file_entry_t *entry,
                                 offline_preroll_plan_t *plan)
{
    if (entry == NULL || plan == NULL) {
        return false;
    }
    memset(plan, 0, sizeof(*plan));
    size_t bytes_read = 0;
    bool eof = false;
    if (!panel_file_manager_read_chunk(index, 0, g_readahead_buf,
                                       sizeof(g_readahead_buf), &bytes_read, &eof) ||
        bytes_read == 0U) {
        return false;
    }
    plan->prefetched_len = bytes_read;
    plan->prefetched_eof = eof;

    if (entry->size_bytes <= PANEL_OFFLINE_PREROLL_REQUEST_BYTES) {
        return true;
    }

    size_t start = PANEL_OFFLINE_PREROLL_REQUEST_BYTES - 1U;
    for (size_t i = start; i < bytes_read; i++) {
        if (g_readahead_buf[i] != '\n') {
            continue;
        }
        uint32_t line_end = (uint32_t)i + 1U;
        if (line_end < entry->size_bytes) {
            plan->effective_offset = line_end;
        }
        return true;
    }

    return entry->size_bytes <= PANEL_OFFLINE_RX_CACHE_SIZE;
}

static errcode_t send_job_begin(uint32_t job_id, uint32_t total_size, uint16_t crc,
                                uint32_t total_lines, uint32_t exec_preroll_bytes)
{
    job_begin_stream_v2_payload_t begin = {0};
    begin.job_id = job_id;
    begin.total_size = total_size;
    begin.job_crc16 = crc;
    begin.options = exec_preroll_bytes > 0U ? JOB_BEGIN_OPTION_AUTO_EXEC_PREROLL : 0U;
    begin.exec_preroll_bytes = exec_preroll_bytes;
    begin.total_lines = total_lines;
    return send_packet_wait_ack(PKT_JOB_BEGIN, &begin, sizeof(begin));
}

static errcode_t send_job_data(uint32_t job_id, uint32_t offset, const uint8_t *data,
                               uint16_t len, uint8_t flags)
{
    uint8_t payload[SLE_JOB_PACKET_MAX_PAYLOAD];
    job_data_payload_t *p = (job_data_payload_t *)payload;
    p->job_id = job_id;
    p->offset = offset;
    p->data_len = len;
    (void)memcpy_s(p->data, PANEL_OFFLINE_CHUNK_MAX, data, len);

    uint16_t payload_len = (uint16_t)(sizeof(job_data_payload_t) + len);
    uint8_t packet[SLE_JOB_PACKET_MAX_SIZE];
    uint16_t packet_len = 0;
    uint16_t seq = panel_job_proto_next_seq();
    g_diag_data_count++;
    uint32_t data_index = g_diag_data_count;

    if (!sle_packet_encode(PKT_JOB_DATA, flags, seq, payload, payload_len,
                           packet, sizeof(packet), &packet_len)) {
        osal_printk("[PANEL_OFFLINE] data encode fail off=%u len=%u\r\n",
                    (unsigned int)offset, (unsigned int)len);
        return ERRCODE_FAIL;
    }

    for (uint32_t retry = 0; retry <= PANEL_OFFLINE_RETRY_MAX; retry++) {
        if (wait_rx_link(PANEL_OFFLINE_CONNECT_WAIT_MS) != ERRCODE_SUCC) {
            osal_printk("[PANEL_OFFLINE] no RX link data off=%u len=%u\r\n",
                        (unsigned int)offset, (unsigned int)len);
            return ERRCODE_SLE_TIMEOUT;
        }
        if (!wait_data_window(seq, data_index, offset, len)) {
            break;
        }

        uint32_t t_send = (uint32_t)uapi_systick_get_ms();
        errcode_t ret = panel_transport_sle_send_rx_packet(packet, packet_len);
        uint32_t send_ms = (uint32_t)uapi_systick_get_ms() - t_send;
        if (ret == ERRCODE_SLE_SUCCESS && !g_data_error) {
            uint32_t next_offset = offset + len;
            uint32_t ack_offset = g_data_ack_offset;
            uint32_t outstanding = (next_offset > ack_offset) ? (next_offset - ack_offset) : 0U;
            if (should_log_packet_timing(PKT_JOB_DATA, data_index, send_ms)) {
                osal_printk("[PANEL_DATA_WIN] t=%u seq=%u data_idx=%u off=%u len=%u "
                            "next=%u send_ms=%u retry=%u ack_off=%u ack_seq=%u "
                            "outstanding=%u window=%u credit=%u link=%u\r\n",
                            (unsigned int)uapi_systick_get_ms(), seq,
                            (unsigned int)data_index, (unsigned int)offset,
                            (unsigned int)len, (unsigned int)next_offset,
                            (unsigned int)send_ms, (unsigned int)retry,
                            (unsigned int)ack_offset,
                            (unsigned int)g_data_ack_seq,
                            (unsigned int)outstanding,
                            (unsigned int)offline_data_window_bytes(),
                            (unsigned int)g_data_ack_credit,
                            (unsigned int)panel_transport_sle_rx_is_connected());
            }
            return ERRCODE_SUCC;
        }

        osal_printk("[PANEL_DATA_SEND_FAIL] t=%u seq=%u data_idx=%u off=%u len=%u "
                    "retry=%u ret=0x%x send_ms=%u rx_err=%u ack_off=%u ack_seq=%u link=%u\r\n",
                    (unsigned int)uapi_systick_get_ms(), seq,
                    (unsigned int)data_index, (unsigned int)offset,
                    (unsigned int)len, (unsigned int)retry,
                    (unsigned int)ret, (unsigned int)send_ms,
                    (unsigned int)g_data_error,
                    (unsigned int)g_data_ack_offset,
                    (unsigned int)g_data_ack_seq,
                    (unsigned int)panel_transport_sle_rx_is_connected());
        if (g_data_error) {
            break;
        }
        osal_msleep(PANEL_OFFLINE_DATA_WINDOW_POLL_MS);
    }

    return ERRCODE_FAIL;
}

static errcode_t send_job_end(uint32_t job_id, uint32_t total_size, uint16_t crc)
{
    job_end_payload_t end = {0};
    end.job_id = job_id;
    end.total_size = total_size;
    end.job_crc16 = crc;
    return send_packet_wait_ack(PKT_JOB_END, &end, sizeof(end));
}

static errcode_t send_exec_start(uint32_t job_id)
{
    exec_start_payload_t start = {0};
    start.job_id = job_id;
    return send_packet_wait_ack(PKT_EXEC_START, &start, sizeof(start));
}

static panel_offline_flow_t handle_priority_control(bool wait_if_paused)
{
    while (1) {
        panel_rx_command_result_t result = panel_rx_commands_dispatch_pending();
        if (result.type != PANEL_RX_COMMAND_NONE) {
            if (result.ret != ERRCODE_SUCC) {
                osal_printk("[PANEL_OFFLINE] control failed type=%u ret=0x%x\r\n",
                            (unsigned int)result.type, result.ret);
                return PANEL_OFFLINE_FLOW_FAIL;
            }
            if (result.type == PANEL_RX_COMMAND_ABORT) {
                panel_model_offline_aborted();
                return PANEL_OFFLINE_FLOW_ABORTED;
            }
            if (result.type == PANEL_RX_COMMAND_EXEC_STOP) {
                panel_model_offline_paused();
            } else if (result.type == PANEL_RX_COMMAND_EXEC_RESUME) {
                panel_model_offline_resumed();
            }
        }

        if (!wait_if_paused || !panel_rx_commands_is_offline_upload_paused()) {
            return PANEL_OFFLINE_FLOW_OK;
        }

        osal_msleep(50);
    }
}

static panel_offline_flow_t upload_selected_file(uint8_t index, const panel_file_entry_t *entry,
                                                 uint16_t crc,
                                                 const offline_preroll_plan_t *plan)
{
    uint32_t offset = 0;
    uint32_t total_size = entry->size_bytes;
    uint32_t packet_count = 0;
    uint32_t next_read_log = 0;
    uint32_t last_status_req_ms = 0;
    uint32_t t_start = (uint32_t)uapi_systick_get_ms();
    bool exec_started = false;
    bool prefetched_pending = plan != NULL && plan->prefetched_len > 0U;
    uint32_t exec_preroll_bytes = (plan != NULL) ? plan->effective_offset : 0U;
    bool rx_auto_exec = exec_preroll_bytes > 0U;
    g_diag_data_count = 0;
    reset_data_ack_state();

    errcode_t ret = send_job_begin(PANEL_OFFLINE_JOB_ID, total_size, crc,
                                   entry->line_count, exec_preroll_bytes);
    if (ret != ERRCODE_SUCC) {
        return PANEL_OFFLINE_FLOW_FAIL;
    }

    osal_printk("[PANEL_OFFLINE] upload pump readahead=%u packet_max=%u window_pkts=%u window=%u\r\n",
                (unsigned int)PANEL_OFFLINE_READAHEAD_BYTES,
                (unsigned int)PANEL_OFFLINE_CHUNK_MAX,
                (unsigned int)PANEL_OFFLINE_DATA_WINDOW_PACKETS,
                (unsigned int)offline_data_window_bytes());

    while (offset < total_size) {
        panel_offline_flow_t flow = handle_priority_control(true);
        if (flow != PANEL_OFFLINE_FLOW_OK) {
            return flow;
        }

        size_t bytes_read = 0;
        bool eof = false;
        uint32_t read_offset = offset;
        uint32_t t_read = (uint32_t)uapi_systick_get_ms();
        bool read_ok;
        if (prefetched_pending && read_offset == 0U) {
            bytes_read = plan->prefetched_len;
            eof = plan->prefetched_eof;
            prefetched_pending = false;
            read_ok = true;
        } else {
            read_ok = panel_file_manager_read_chunk(index, read_offset, g_readahead_buf,
                                                    sizeof(g_readahead_buf), &bytes_read, &eof);
        }
        uint32_t read_ms = (uint32_t)uapi_systick_get_ms() - t_read;
        if (!read_ok || bytes_read == 0U) {
            const panel_file_manager_t *mgr = panel_file_manager_get();
            const char *err = (mgr != NULL) ? mgr->last_error : "unknown";
            osal_printk("[PANEL_OFFLINE_READ_FAIL] off=%u req=%u got=%u eof=%u ms=%u err=%s\r\n",
                        (unsigned int)read_offset,
                        (unsigned int)sizeof(g_readahead_buf),
                        (unsigned int)bytes_read,
                        eof ? 1U : 0U,
                        (unsigned int)read_ms,
                        err);
            return PANEL_OFFLINE_FLOW_FAIL;
        }
        if (read_offset >= next_read_log || read_ms >= PANEL_OFFLINE_READ_SLOW_MS || eof) {
            osal_printk("[PANEL_OFFLINE_READ] off=%u req=%u got=%u eof=%u ms=%u\r\n",
                        (unsigned int)read_offset,
                        (unsigned int)sizeof(g_readahead_buf),
                        (unsigned int)bytes_read,
                        eof ? 1U : 0U,
                        (unsigned int)read_ms);
            while (next_read_log <= read_offset) {
                next_read_log += PANEL_OFFLINE_READ_LOG_BYTES;
            }
        }

        size_t read_pos = 0;
        while (read_pos < bytes_read) {
            flow = handle_priority_control(true);
            if (flow != PANEL_OFFLINE_FLOW_OK) {
                return flow;
            }

            size_t remain = bytes_read - read_pos;
            uint16_t chunk_len = (remain > PANEL_OFFLINE_CHUNK_MAX) ?
                (uint16_t)PANEL_OFFLINE_CHUNK_MAX : (uint16_t)remain;
            const uint8_t *chunk = &g_readahead_buf[read_pos];

            if (rx_auto_exec && !exec_started && offset < exec_preroll_bytes &&
                offset + chunk_len > exec_preroll_bytes) {
                chunk_len = (uint16_t)(exec_preroll_bytes - offset);
            }
            uint8_t data_flags = SLE_JOB_PACKET_FLAG_DATA_FAST_ACK;
            bool starts_exec_after_chunk = rx_auto_exec && !exec_started &&
                offset + chunk_len >= exec_preroll_bytes;
            /*
             * RX emits a cumulative fast ACK every 3 * 300 bytes.  A short
             * packet at a readahead/file boundary can leave fewer than 900
             * bytes received while the next 300-byte packet would exceed the
             * Screen's 900-byte window.  Force an ACK on that boundary packet
             * so sender and receiver cannot wait on each other indefinitely.
             */
            if (starts_exec_after_chunk || chunk_len < PANEL_OFFLINE_CHUNK_MAX) {
                data_flags |= SLE_JOB_PACKET_FLAG_DATA_FORCE_ACK;
            }

            ret = send_job_data(PANEL_OFFLINE_JOB_ID, offset, chunk, chunk_len, data_flags);
            if (ret != ERRCODE_SUCC) {
                return PANEL_OFFLINE_FLOW_FAIL;
            }

            flow = handle_priority_control(true);
            if (flow != PANEL_OFFLINE_FLOW_OK) {
                return flow;
            }

            offset += (uint32_t)chunk_len;
            read_pos += chunk_len;
            packet_count++;

            if (starts_exec_after_chunk) {
                /*
                 * DATA_FORCE_ACK is the RX auto-exec barrier.  Do not put later
                 * DATA into the write-cmd window until RX has cumulatively
                 * accepted the complete preroll boundary.
                 */
                if (!wait_data_drain(offset)) {
                    osal_printk("[PANEL_OFFLINE] RX auto exec threshold ack failed offset=%u\r\n",
                                (unsigned int)offset);
                    return PANEL_OFFLINE_FLOW_FAIL;
                }
                exec_started = true;
                panel_model_offline_execution_started();
                osal_printk("[PANEL_OFFLINE] RX auto exec threshold acked offset=%u ack_seq=%u\r\n",
                            (unsigned int)offset, (unsigned int)g_data_ack_seq);
            }

            if ((packet_count % PANEL_OFFLINE_PROGRESS_PACKETS) == 0U || offset >= total_size) {
                panel_model_offline_upload_progress(offset);
            }
            uint32_t now_ms = (uint32_t)uapi_systick_get_ms();
            if (exec_started &&
                (last_status_req_ms == 0U ||
                 (uint32_t)(now_ms - last_status_req_ms) >= PANEL_OFFLINE_STATUS_MS)) {
                (void)send_packet_no_ack(PKT_STATUS_REQ, NULL, 0);
                last_status_req_ms = now_ms;
            }
        }

        if (eof && offset < total_size) {
            return PANEL_OFFLINE_FLOW_FAIL;
        }
    }

    panel_offline_flow_t flow = handle_priority_control(true);
    if (flow != PANEL_OFFLINE_FLOW_OK) {
        return flow;
    }

    if (!wait_data_drain(total_size)) {
        return PANEL_OFFLINE_FLOW_FAIL;
    }

    ret = send_job_end(PANEL_OFFLINE_JOB_ID, total_size, crc);
    if (ret != ERRCODE_SUCC) {
        return PANEL_OFFLINE_FLOW_FAIL;
    }

    if (!rx_auto_exec) {
        flow = handle_priority_control(true);
        if (flow != PANEL_OFFLINE_FLOW_OK) {
            return flow;
        }
        ret = send_exec_start(PANEL_OFFLINE_JOB_ID);
        if (ret != ERRCODE_SUCC) {
            return PANEL_OFFLINE_FLOW_FAIL;
        }
        panel_model_offline_execution_started();
        osal_printk("[PANEL_OFFLINE] exec start after full upload\r\n");
    } else if (!exec_started) {
        return PANEL_OFFLINE_FLOW_FAIL;
    }

    uint32_t elapsed = (uint32_t)uapi_systick_get_ms() - t_start;
    uint32_t rate = (elapsed > 0U) ? ((total_size * 1000U) / elapsed) : 0U;
    osal_printk("[PANEL_OFFLINE] upload done bytes=%u packets=%u elapsed=%ums rate=%uB/s\r\n",
                (unsigned int)total_size, (unsigned int)packet_count,
                (unsigned int)elapsed, (unsigned int)rate);

    return PANEL_OFFLINE_FLOW_OK;
}

static void poll_execution_until_idle(void)
{
    uint32_t start = (uint32_t)uapi_systick_get_ms();
    uint32_t last_status = 0;
    bool saw_executing = false;

    while ((uint32_t)((uint32_t)uapi_systick_get_ms() - start) < PANEL_OFFLINE_EXEC_TIMEOUT_MS) {
        panel_offline_flow_t flow = handle_priority_control(true);
        if (flow == PANEL_OFFLINE_FLOW_ABORTED) {
            osal_printk("[PANEL_OFFLINE] execution poll aborted by screen control\r\n");
            return;
        }
        if (flow != PANEL_OFFLINE_FLOW_OK) {
            panel_model_offline_error("CTRL_FAIL");
            return;
        }

        uint32_t now = (uint32_t)uapi_systick_get_ms();
        if (last_status == 0U || (uint32_t)(now - last_status) >= PANEL_OFFLINE_STATUS_MS) {
            (void)send_packet_no_ack(PKT_STATUS_REQ, NULL, 0);
            last_status = now;
        }

        if (g_model.state == SYS_STATE_RUNNING) {
            saw_executing = true;
        }
        if (g_model.state == SYS_STATE_DONE ||
            (saw_executing && g_model.state == SYS_STATE_NO_JOB)) {
            osal_printk("[PANEL_OFFLINE] execution complete state=%s\r\n",
                        panel_model_state_text(g_model.state));
            panel_model_offline_job_done();
            return;
        }
        if (g_model.state == SYS_STATE_ERROR || g_model.state == SYS_STATE_LINK_LOST) {
            return;
        }
        osal_msleep(50);
    }

    panel_model_offline_error("EXEC_TIMEOUT");
}

static void run_offline_job(uint8_t index)
{
    const panel_file_entry_t *entry = panel_file_manager_get_entry(index);
    if (entry == NULL || !entry->selectable) {
        panel_model_offline_error("NO_FILE");
        osal_printk("[PANEL_OFFLINE] rejected: no selectable file index=%u\r\n",
                    (unsigned int)index);
        return;
    }
    if (!panel_transport_sle_can_control_rx()) {
        panel_model_offline_error("DISPLAY_ONLY");
        osal_printk("[PANEL_OFFLINE] rejected: tx mirror present, display-only mode\r\n");
        return;
    }
    if (entry->size_bytes == 0U) {
        panel_model_offline_error("BAD_SIZE");
        osal_printk("[PANEL_OFFLINE] rejected: zero size file=%s\r\n", entry->name);
        return;
    }

    uint32_t total_lines = entry->line_count;
    if (total_lines == 0U) {
        uint32_t scan_start_ms = (uint32_t)uapi_systick_get_ms();
        osal_printk("[PANEL_OFFLINE] line scan start file=%s size=%u\r\n",
                    entry->name, (unsigned int)entry->size_bytes);
        if (!panel_file_manager_ensure_line_count(index, &total_lines)) {
            panel_model_offline_error("LINE_SCAN_FAIL");
            osal_printk("[PANEL_OFFLINE] line scan failed file=%s\r\n", entry->name);
            return;
        }
        osal_printk("[PANEL_OFFLINE] line scan done file=%s lines=%u elapsed=%ums\r\n",
                    entry->name, (unsigned int)total_lines,
                    (unsigned int)((uint32_t)uapi_systick_get_ms() - scan_start_ms));
    }
    if (total_lines == 0U) {
        panel_model_offline_error("NO_GCODE_LINES");
        osal_printk("[PANEL_OFFLINE] rejected: no executable lines file=%s\r\n",
                    entry->name);
        return;
    }

    panel_model_offline_upload_begin(entry->name, entry->size_bytes, total_lines);

    uint16_t crc = 0;
#if PANEL_OFFLINE_USE_FULL_FILE_CRC
    osal_printk("[PANEL_OFFLINE] crc start file=%s size=%u\r\n",
                entry->name, (unsigned int)entry->size_bytes);
    if (compute_file_crc(index, entry->size_bytes, &crc) != ERRCODE_SUCC) {
        panel_model_offline_error("READ_CRC_FAIL");
        return;
    }
#else
    osal_printk("[PANEL_OFFLINE] full file crc disabled, streaming with packet crc\r\n");
#endif

    offline_preroll_plan_t plan;
    if (!plan_offline_preroll(index, entry, &plan)) {
        panel_model_offline_error("PREROLL_BOUNDARY");
        osal_printk("[PANEL_OFFLINE] no safe preroll line boundary size=%u scan=%u\r\n",
                    (unsigned int)entry->size_bytes,
                    (unsigned int)sizeof(g_readahead_buf));
        return;
    }

    osal_printk("[PANEL_OFFLINE] begin v2 file=%s size=%u lines=%u crc=0x%04x "
                "preroll_req=%u preroll_effective=%u prefetched=%u\r\n",
                entry->name, (unsigned int)entry->size_bytes,
                (unsigned int)entry->line_count, crc,
                (unsigned int)PANEL_OFFLINE_PREROLL_REQUEST_BYTES,
                (unsigned int)plan.effective_offset,
                (unsigned int)plan.prefetched_len);

    panel_offline_flow_t flow = upload_selected_file(index, entry, crc, &plan);
    if (flow == PANEL_OFFLINE_FLOW_ABORTED) {
        osal_printk("[PANEL_OFFLINE] upload aborted by screen control\r\n");
        return;
    }
    if (flow != PANEL_OFFLINE_FLOW_OK) {
        abort_rx_best_effort();
        panel_model_offline_error("UPLOAD_FAIL");
        return;
    }

    panel_model_offline_upload_progress(entry->size_bytes);
    poll_execution_until_idle();
}

static int panel_offline_job_task(void *arg)
{
    (void)arg;
    g_worker_ready = true;
    osal_printk("[PANEL_OFFLINE_TASK] ready\r\n");
    while (1) {
        if (osal_sem_down(&g_start_sem) != OSAL_SUCCESS) {
            osal_printk("[PANEL_OFFLINE_TASK] start semaphore wait failed\r\n");
            osal_msleep(20);
            continue;
        }
        if (!g_start_requested) {
            continue;
        }

        uint32_t lock = osal_irq_lock();
        uint8_t index = g_pending_index;
        g_busy = true;
        g_start_requested = false;
        osal_irq_restore(lock);
        osal_printk("[PANEL_OFFLINE_TASK] dispatch index=%u rx=%u\r\n",
                    (unsigned int)index,
                    (unsigned int)(panel_transport_sle_rx_is_connected() ? 1U : 0U));
        panel_transport_sle_set_standalone_session_active(true);
        panel_rx_commands_set_offline_upload_active(true);
        run_offline_job(index);
        panel_rx_commands_set_offline_upload_active(false);
        panel_transport_sle_set_standalone_session_active(false);
        g_busy = false;
    }
    return 0;
}

errcode_t panel_offline_job_init(void)
{
    if (!g_ack_sem_ready && osal_sem_init(&g_ack_sem, 0) != OSAL_SUCCESS) {
        return ERRCODE_FAIL;
    }
    if (!g_ack_sem_ready) {
        g_ack_sem_ready = true;
    }
    if (!g_start_sem_ready && osal_sem_init(&g_start_sem, 0) != OSAL_SUCCESS) {
        return ERRCODE_FAIL;
    }
    if (!g_start_sem_ready) {
        g_start_sem_ready = true;
    }
    panel_transport_sle_set_rx_response_cb(rx_response_cb);
    return task_create("panel_offline_job", panel_offline_job_task, NULL,
                       PANEL_OFFLINE_TASK_STACK_SIZE, PANEL_OFFLINE_TASK_PRIORITY);
}

errcode_t panel_offline_job_start_selected(void)
{
    const panel_file_manager_t *mgr = panel_file_manager_get();
    if (mgr == NULL || mgr->selected_index < 0 ||
        !g_worker_ready || !g_start_sem_ready ||
        !panel_transport_sle_rx_is_connected() ||
        !panel_transport_sle_can_control_rx()) {
        return ERRCODE_FAIL;
    }

    uint32_t lock = osal_irq_lock();
    if (g_busy || g_start_requested) {
        osal_irq_restore(lock);
        return ERRCODE_FAIL;
    }
    g_pending_index = (uint8_t)mgr->selected_index;
    g_start_requested = true;
    osal_irq_restore(lock);

    osal_sem_up(&g_start_sem);
    osal_printk("[PANEL_OFFLINE] start request index=%u rx=1 worker=1\r\n",
                (unsigned int)g_pending_index);
    return ERRCODE_SUCC;
}

bool panel_offline_job_is_ready(void)
{
    return g_worker_ready && g_start_sem_ready;
}

bool panel_offline_job_is_busy(void)
{
    return g_busy || g_start_requested;
}
