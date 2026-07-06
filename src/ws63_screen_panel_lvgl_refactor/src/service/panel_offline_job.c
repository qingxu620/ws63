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
#define PANEL_OFFLINE_YIELD_PACKETS   16U
#define PANEL_OFFLINE_PROGRESS_PACKETS 4U
#define PANEL_OFFLINE_PREROLL_REQUEST_BYTES 4096U
#define PANEL_OFFLINE_PREROLL_FALLBACK_BYTES 8192U
#define PANEL_OFFLINE_PREROLL_LINE_MAX 128U
#define PANEL_OFFLINE_TIMING_FIRST_PACKETS 0U
#define PANEL_OFFLINE_TIMING_EVERY_PACKETS 0U
#define PANEL_OFFLINE_TIMING_SLOW_MS 500U
#define PANEL_OFFLINE_READ_LOG_BYTES 32768U
#define PANEL_OFFLINE_READ_SLOW_MS 100U

_Static_assert(sizeof(job_data_payload_t) + PANEL_OFFLINE_CHUNK_MAX <= SLE_JOB_PACKET_MAX_PAYLOAD,
               "PANEL_OFFLINE_CHUNK_MAX too large for SLE payload");

typedef struct {
    uint32_t request_offset;
    uint32_t fallback_offset;
    uint8_t line[PANEL_OFFLINE_PREROLL_LINE_MAX];
    uint16_t line_len;
    bool triggered;
} preroll_tracker_t;

typedef enum {
    PANEL_OFFLINE_FLOW_OK = 0,
    PANEL_OFFLINE_FLOW_FAIL,
    PANEL_OFFLINE_FLOW_ABORTED,
} panel_offline_flow_t;

typedef enum {
    PANEL_OFFLINE_REQUEST_FILE = 0,
    PANEL_OFFLINE_REQUEST_FRAME_SCAN,
} panel_offline_request_t;

static const uint8_t g_frame_scan_gcode[] =
    "M5\n"
    "G90\n"
    "G0 X0 Y0 F100000\n"
    "M3 S200\n"
    "G1 X99 Y0 F500\n"
    "G1 X99 Y99 F500\n"
    "G1 X0 Y99 F500\n"
    "G1 X0 Y0 F500\n"
    "M5\n"
    "M30\n";

static osal_semaphore g_ack_sem;
static volatile bool g_ack_sem_ready;
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
static volatile panel_offline_request_t g_pending_request;
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
    uint16_t seq = panel_job_proto_next_seq();
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

static char ascii_upper(char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        return (char)(ch - ('a' - 'A'));
    }
    return ch;
}

static bool token_is_laser_off(const uint8_t *line, uint16_t start, uint16_t end)
{
    if (start >= end) {
        return false;
    }

    char first = ascii_upper((char)line[start]);
    if (first == 'M') {
        return (end == (uint16_t)(start + 2U) && line[start + 1U] == '5');
    }

    if (first != 'S' || end <= (uint16_t)(start + 1U)) {
        return false;
    }

    for (uint16_t i = (uint16_t)(start + 1U); i < end; i++) {
        char ch = (char)line[i];
        if (ch == '+' || ch == '-' || ch == '.') {
            continue;
        }
        if (ch != '0') {
            return false;
        }
    }
    return true;
}

static bool line_final_laser_state_is_off(const uint8_t *line, uint16_t len)
{
    bool saw_laser_state = false;
    bool laser_off = false;
    uint16_t token_start = UINT16_MAX;

    for (uint16_t i = 0; i <= len; i++) {
        char ch = (i < len) ? (char)line[i] : ' ';
        if (ch == ';' || ch == '(') {
            ch = ' ';
            len = i;
        }

        bool sep = (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n');
        if (!sep && token_start == UINT16_MAX) {
            token_start = i;
        }
        if ((sep || i == len) && token_start != UINT16_MAX) {
            uint16_t token_end = i;
            char first = ascii_upper((char)line[token_start]);
            if (first == 'M' || first == 'S') {
                saw_laser_state = true;
                laser_off = token_is_laser_off(line, token_start, token_end);
            }
            token_start = UINT16_MAX;
        }
    }

    return saw_laser_state && laser_off;
}

static void preroll_tracker_init(preroll_tracker_t *tracker, uint32_t total_size)
{
    memset(tracker, 0, sizeof(*tracker));
    tracker->request_offset = PANEL_OFFLINE_PREROLL_REQUEST_BYTES;
    tracker->fallback_offset = PANEL_OFFLINE_PREROLL_FALLBACK_BYTES;
    if (tracker->fallback_offset < tracker->request_offset) {
        tracker->fallback_offset = tracker->request_offset;
    }
    if (total_size <= tracker->request_offset) {
        tracker->triggered = true;
    }
}

static bool preroll_tracker_consume(preroll_tracker_t *tracker, const uint8_t *data,
                                    uint16_t len, uint32_t chunk_offset,
                                    uint32_t *trigger_offset)
{
    if (tracker == NULL || data == NULL || tracker->triggered) {
        return false;
    }

    for (uint16_t i = 0; i < len; i++) {
        uint8_t ch = data[i];
        uint32_t absolute_offset = chunk_offset + i + 1U;
        bool line_end = (ch == '\n' || ch == '\r');

        if (!line_end && tracker->line_len < (PANEL_OFFLINE_PREROLL_LINE_MAX - 1U)) {
            tracker->line[tracker->line_len++] = ch;
        }

        if (!line_end) {
            continue;
        }

        bool safe_laser_boundary =
            absolute_offset >= tracker->request_offset &&
            line_final_laser_state_is_off(tracker->line, tracker->line_len);
        bool fallback_boundary = absolute_offset >= tracker->fallback_offset;
        tracker->line_len = 0;

        if (safe_laser_boundary || fallback_boundary) {
            tracker->triggered = true;
            if (trigger_offset != NULL) {
                *trigger_offset = absolute_offset;
            }
            return true;
        }
    }

    return false;
}

static errcode_t send_job_begin(uint32_t job_id, uint32_t total_size, uint16_t crc)
{
    job_begin_payload_t begin = {0};
    begin.job_id = job_id;
    begin.total_size = total_size;
    begin.job_crc16 = crc;
    return send_packet_wait_ack(PKT_JOB_BEGIN, &begin, sizeof(begin));
}

static errcode_t send_job_data(uint32_t job_id, uint32_t offset, const uint8_t *data, uint16_t len)
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

    if (!sle_packet_encode(PKT_JOB_DATA, 0, seq, payload, payload_len,
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

static panel_offline_flow_t upload_selected_file(uint8_t index, const panel_file_entry_t *entry, uint16_t crc)
{
    uint32_t offset = 0;
    uint32_t total_size = entry->size_bytes;
    uint32_t packet_count = 0;
    uint32_t next_read_log = 0;
    uint32_t t_start = (uint32_t)uapi_systick_get_ms();
    bool exec_started = false;
    preroll_tracker_t preroll;
    preroll_tracker_init(&preroll, total_size);
    g_diag_data_count = 0;
    reset_data_ack_state();

    errcode_t ret = send_job_begin(PANEL_OFFLINE_JOB_ID, total_size, crc);
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
        bool read_ok = panel_file_manager_read_chunk(index, read_offset, g_readahead_buf,
                                                     sizeof(g_readahead_buf), &bytes_read, &eof);
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

            ret = send_job_data(PANEL_OFFLINE_JOB_ID, offset, chunk, chunk_len);
            if (ret != ERRCODE_SUCC) {
                return PANEL_OFFLINE_FLOW_FAIL;
            }

            flow = handle_priority_control(true);
            if (flow != PANEL_OFFLINE_FLOW_OK) {
                return flow;
            }

            uint32_t trigger_offset = 0;
            if (!exec_started &&
                preroll_tracker_consume(&preroll, chunk, chunk_len, offset, &trigger_offset)) {
                osal_printk("[PANEL_OFFLINE] preroll ready offset=%u request=%u fallback=%u\r\n",
                            (unsigned int)trigger_offset,
                            (unsigned int)preroll.request_offset,
                            (unsigned int)preroll.fallback_offset);
                if (!wait_data_drain(trigger_offset)) {
                    return PANEL_OFFLINE_FLOW_FAIL;
                }
                ret = send_exec_start(PANEL_OFFLINE_JOB_ID);
                if (ret != ERRCODE_SUCC) {
                    return PANEL_OFFLINE_FLOW_FAIL;
                }
                exec_started = true;
                panel_model_offline_execution_started();
                osal_printk("[PANEL_OFFLINE] exec start at upload_offset=%u\r\n",
                            (unsigned int)(offset + (uint32_t)chunk_len));
            }

            offset += (uint32_t)chunk_len;
            read_pos += chunk_len;
            packet_count++;

            if ((packet_count % PANEL_OFFLINE_PROGRESS_PACKETS) == 0U || offset >= total_size) {
                panel_model_offline_upload_progress(offset);
            }
            if ((packet_count % PANEL_OFFLINE_YIELD_PACKETS) == 0U) {
                osal_yield();
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

    if (!exec_started) {
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
    }

    uint32_t elapsed = (uint32_t)uapi_systick_get_ms() - t_start;
    uint32_t rate = (elapsed > 0U) ? ((total_size * 1000U) / elapsed) : 0U;
    osal_printk("[PANEL_OFFLINE] upload done bytes=%u packets=%u elapsed=%ums rate=%uB/s\r\n",
                (unsigned int)total_size, (unsigned int)packet_count,
                (unsigned int)elapsed, (unsigned int)rate);

    return PANEL_OFFLINE_FLOW_OK;
}

static panel_offline_flow_t upload_memory_job(const char *name, const uint8_t *data,
                                              uint32_t total_size, uint16_t crc)
{
    uint32_t offset = 0;
    uint32_t packet_count = 0;
    uint32_t t_start = (uint32_t)uapi_systick_get_ms();
    bool exec_started = false;
    preroll_tracker_t preroll;
    preroll_tracker_init(&preroll, total_size);
    g_diag_data_count = 0;
    reset_data_ack_state();

    errcode_t ret = send_job_begin(PANEL_OFFLINE_JOB_ID, total_size, crc);
    if (ret != ERRCODE_SUCC) {
        return PANEL_OFFLINE_FLOW_FAIL;
    }

    while (offset < total_size) {
        panel_offline_flow_t flow = handle_priority_control(true);
        if (flow != PANEL_OFFLINE_FLOW_OK) {
            return flow;
        }

        uint32_t remain = total_size - offset;
        uint16_t chunk_len = (remain > PANEL_OFFLINE_CHUNK_MAX) ?
            (uint16_t)PANEL_OFFLINE_CHUNK_MAX : (uint16_t)remain;
        const uint8_t *chunk = &data[offset];

        ret = send_job_data(PANEL_OFFLINE_JOB_ID, offset, chunk, chunk_len);
        if (ret != ERRCODE_SUCC) {
            return PANEL_OFFLINE_FLOW_FAIL;
        }

        uint32_t trigger_offset = 0;
        if (!exec_started &&
            preroll_tracker_consume(&preroll, chunk, chunk_len, offset, &trigger_offset)) {
            if (!wait_data_drain(trigger_offset)) {
                return PANEL_OFFLINE_FLOW_FAIL;
            }
            ret = send_exec_start(PANEL_OFFLINE_JOB_ID);
            if (ret != ERRCODE_SUCC) {
                return PANEL_OFFLINE_FLOW_FAIL;
            }
            exec_started = true;
            panel_model_offline_execution_started();
            osal_printk("[PANEL_OFFLINE] exec start memory job=%s at off=%u\r\n",
                        name, (unsigned int)(offset + (uint32_t)chunk_len));
        }

        offset += (uint32_t)chunk_len;
        packet_count++;
        panel_model_offline_upload_progress(offset);
        if ((packet_count % PANEL_OFFLINE_YIELD_PACKETS) == 0U) {
            osal_yield();
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

    if (!exec_started) {
        ret = send_exec_start(PANEL_OFFLINE_JOB_ID);
        if (ret != ERRCODE_SUCC) {
            return PANEL_OFFLINE_FLOW_FAIL;
        }
        panel_model_offline_execution_started();
    }

    uint32_t elapsed = (uint32_t)uapi_systick_get_ms() - t_start;
    osal_printk("[PANEL_OFFLINE] memory upload done job=%s bytes=%u packets=%u elapsed=%ums\r\n",
                name, (unsigned int)total_size, (unsigned int)packet_count,
                (unsigned int)elapsed);
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
        if (saw_executing && g_model.state == SYS_STATE_NO_JOB) {
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
        return;
    }
    if (!panel_transport_sle_can_control_rx()) {
        panel_model_offline_error("DISPLAY_ONLY");
        osal_printk("[PANEL_OFFLINE] rejected: tx mirror present, display-only mode\r\n");
        return;
    }
    if (entry->size_bytes == 0U) {
        panel_model_offline_error("BAD_SIZE");
        return;
    }

    panel_model_offline_upload_begin(entry->name, entry->size_bytes, entry->line_count);

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

    osal_printk("[PANEL_OFFLINE] begin file=%s size=%u crc=0x%04x preroll=%u\r\n",
                entry->name, (unsigned int)entry->size_bytes, crc,
                (unsigned int)PANEL_OFFLINE_PREROLL_REQUEST_BYTES);

    panel_offline_flow_t flow = upload_selected_file(index, entry, crc);
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

static void run_frame_scan_job(void)
{
    if (!panel_transport_sle_can_control_rx()) {
        panel_model_offline_error("DISPLAY_ONLY");
        osal_printk("[PANEL_OFFLINE] frame rejected: tx mirror present, display-only mode\r\n");
        return;
    }

    const uint32_t total_size = (uint32_t)(sizeof(g_frame_scan_gcode) - 1U);
    panel_model_offline_upload_begin("SCAN_FRAME", total_size, 10U);
    osal_printk("[PANEL_OFFLINE] frame scan begin size=%u\r\n", (unsigned int)total_size);

    panel_offline_flow_t flow = upload_memory_job("SCAN_FRAME", g_frame_scan_gcode, total_size, 0);
    if (flow == PANEL_OFFLINE_FLOW_ABORTED) {
        osal_printk("[PANEL_OFFLINE] frame scan aborted by screen control\r\n");
        return;
    }
    if (flow != PANEL_OFFLINE_FLOW_OK) {
        abort_rx_best_effort();
        panel_model_offline_error("FRAME_FAIL");
        return;
    }

    panel_model_offline_upload_progress(total_size);
    poll_execution_until_idle();
}

static int panel_offline_job_task(void *arg)
{
    (void)arg;
    while (1) {
        panel_transport_sle_poll();
        if (!g_start_requested) {
            osal_msleep(50);
            continue;
        }

        g_start_requested = false;
        g_busy = true;
        panel_transport_sle_set_standalone_session_active(true);
        panel_rx_commands_set_offline_upload_active(true);
        if (g_pending_request == PANEL_OFFLINE_REQUEST_FRAME_SCAN) {
            run_frame_scan_job();
        } else {
            run_offline_job(g_pending_index);
        }
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
    panel_transport_sle_set_rx_response_cb(rx_response_cb);
    return task_create("panel_offline_job", panel_offline_job_task, NULL,
                       PANEL_OFFLINE_TASK_STACK_SIZE, PANEL_OFFLINE_TASK_PRIORITY);
}

errcode_t panel_offline_job_start_selected(void)
{
    const panel_file_manager_t *mgr = panel_file_manager_get();
    if (mgr == NULL || mgr->selected_index < 0 || g_busy || g_start_requested ||
        !panel_transport_sle_can_control_rx()) {
        return ERRCODE_FAIL;
    }
    g_pending_index = (uint8_t)mgr->selected_index;
    g_pending_request = PANEL_OFFLINE_REQUEST_FILE;
    g_start_requested = true;
    osal_printk("[PANEL_OFFLINE] start request index=%u\r\n", (unsigned int)g_pending_index);
    return ERRCODE_SUCC;
}

errcode_t panel_offline_job_start_frame_scan(void)
{
    if (g_busy || g_start_requested || !panel_transport_sle_can_control_rx()) {
        return ERRCODE_FAIL;
    }
    g_pending_index = 0;
    g_pending_request = PANEL_OFFLINE_REQUEST_FRAME_SCAN;
    g_start_requested = true;
    osal_printk("[PANEL_OFFLINE] frame scan request\r\n");
    return ERRCODE_SUCC;
}

bool panel_offline_job_is_busy(void)
{
    return g_busy || g_start_requested;
}
