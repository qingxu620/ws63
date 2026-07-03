/**
 * @file panel_offline_job.c
 * @brief Reads selected SD G-code and sends it to RX using SLE Job packets.
 */
#include "panel_offline_job.h"
#include "panel_file_manager.h"
#include "panel_job_proto.h"
#include "panel_model.h"
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
#define PANEL_OFFLINE_TASK_PRIORITY   24
#define PANEL_OFFLINE_JOB_ID          2U
#define PANEL_OFFLINE_JOB_MAX_SIZE    131072U
#define PANEL_OFFLINE_CONNECT_WAIT_MS 8000U
#define PANEL_OFFLINE_ACK_TIMEOUT_MS  1000U
#define PANEL_OFFLINE_RETRY_MAX       8U
#define PANEL_OFFLINE_STATUS_MS       1000U
#define PANEL_OFFLINE_EXEC_TIMEOUT_MS 600000U
#define PANEL_OFFLINE_CHUNK_MAX       (SLE_JOB_PACKET_MAX_PAYLOAD - sizeof(job_data_payload_t))
#define PANEL_OFFLINE_USE_FULL_FILE_CRC 0
#define PANEL_OFFLINE_READAHEAD_BYTES 8192U
#define PANEL_OFFLINE_YIELD_PACKETS   16U
#define PANEL_OFFLINE_PROGRESS_PACKETS 4U
#define PANEL_OFFLINE_PREROLL_REQUEST_BYTES 4096U
#define PANEL_OFFLINE_PREROLL_FALLBACK_BYTES 8192U
#define PANEL_OFFLINE_PREROLL_LINE_MAX 128U

typedef struct {
    uint32_t request_offset;
    uint32_t fallback_offset;
    uint8_t line[PANEL_OFFLINE_PREROLL_LINE_MAX];
    uint16_t line_len;
    bool triggered;
} preroll_tracker_t;

static osal_semaphore g_ack_sem;
static volatile bool g_ack_sem_ready;
static volatile bool g_wait_active;
static volatile bool g_wait_got_ack;
static volatile uint16_t g_wait_ack_seq;
static volatile uint8_t g_wait_status;
static volatile bool g_busy;
static volatile bool g_start_requested;
static uint8_t g_pending_index;
static uint8_t g_readahead_buf[PANEL_OFFLINE_READAHEAD_BYTES];

static uint16_t payload_len_for_type(uint8_t type, const void *payload, uint16_t fallback)
{
    if (type == PKT_JOB_DATA && payload != NULL) {
        const job_data_payload_t *p = (const job_data_payload_t *)payload;
        return (uint16_t)(sizeof(job_data_payload_t) + p->data_len);
    }
    return fallback;
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

    g_wait_ack_seq = seq;
    g_wait_active = true;

    for (uint32_t retry = 0; retry <= PANEL_OFFLINE_RETRY_MAX; retry++) {
        while (g_ack_sem_ready && osal_sem_down_timeout(&g_ack_sem, 0) == OSAL_SUCCESS) {
        }
        g_wait_status = JOB_STATUS_INTERNAL_ERROR;
        g_wait_got_ack = false;
        errcode_t ret = panel_transport_sle_send_rx_packet(packet, packet_len);
        if (ret == ERRCODE_SLE_SUCCESS &&
            osal_sem_down_timeout(&g_ack_sem, PANEL_OFFLINE_ACK_TIMEOUT_MS) == OSAL_SUCCESS &&
            g_wait_got_ack) {
            g_wait_active = false;
            g_wait_ack_seq = 0;
            return (g_wait_status == JOB_STATUS_OK) ? ERRCODE_SUCC : ERRCODE_FAIL;
        }
        osal_printk("[PANEL_OFFLINE] wait ack retry type=0x%02x seq=%u try=%u st=%u\r\n",
                    type, (unsigned int)seq, (unsigned int)retry, (unsigned int)g_wait_status);
    }

    g_wait_active = false;
    g_wait_ack_seq = 0;
    return ERRCODE_SLE_TIMEOUT;
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
    return send_packet_wait_ack(PKT_JOB_DATA, payload,
                                (uint16_t)(sizeof(job_data_payload_t) + len));
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

static errcode_t upload_selected_file(uint8_t index, const panel_file_entry_t *entry, uint16_t crc)
{
    uint32_t offset = 0;
    uint32_t total_size = entry->size_bytes;
    uint32_t packet_count = 0;
    uint32_t t_start = (uint32_t)uapi_systick_get_ms();
    bool exec_started = false;
    preroll_tracker_t preroll;
    preroll_tracker_init(&preroll, total_size);

    errcode_t ret = send_job_begin(PANEL_OFFLINE_JOB_ID, total_size, crc);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    osal_printk("[PANEL_OFFLINE] upload pump readahead=%u packet_max=%u\r\n",
                (unsigned int)PANEL_OFFLINE_READAHEAD_BYTES,
                (unsigned int)PANEL_OFFLINE_CHUNK_MAX);

    while (offset < total_size) {
        size_t bytes_read = 0;
        bool eof = false;
        uint32_t read_offset = offset;
        if (!panel_file_manager_read_chunk(index, read_offset, g_readahead_buf,
                                           sizeof(g_readahead_buf), &bytes_read, &eof) ||
            bytes_read == 0U) {
            return ERRCODE_FAIL;
        }

        size_t read_pos = 0;
        while (read_pos < bytes_read) {
            size_t remain = bytes_read - read_pos;
            uint16_t chunk_len = (remain > PANEL_OFFLINE_CHUNK_MAX) ?
                (uint16_t)PANEL_OFFLINE_CHUNK_MAX : (uint16_t)remain;
            const uint8_t *chunk = &g_readahead_buf[read_pos];

            ret = send_job_data(PANEL_OFFLINE_JOB_ID, offset, chunk, chunk_len);
            if (ret != ERRCODE_SUCC) {
                return ret;
            }

            uint32_t trigger_offset = 0;
            if (!exec_started &&
                preroll_tracker_consume(&preroll, chunk, chunk_len, offset, &trigger_offset)) {
                osal_printk("[PANEL_OFFLINE] preroll ready offset=%u request=%u fallback=%u\r\n",
                            (unsigned int)trigger_offset,
                            (unsigned int)preroll.request_offset,
                            (unsigned int)preroll.fallback_offset);
                ret = send_exec_start(PANEL_OFFLINE_JOB_ID);
                if (ret != ERRCODE_SUCC) {
                    return ret;
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
            return ERRCODE_FAIL;
        }
    }

    ret = send_job_end(PANEL_OFFLINE_JOB_ID, total_size, crc);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    if (!exec_started) {
        ret = send_exec_start(PANEL_OFFLINE_JOB_ID);
        if (ret != ERRCODE_SUCC) {
            return ret;
        }
        panel_model_offline_execution_started();
        osal_printk("[PANEL_OFFLINE] exec start after full upload\r\n");
    }

    uint32_t elapsed = (uint32_t)uapi_systick_get_ms() - t_start;
    uint32_t rate = (elapsed > 0U) ? ((total_size * 1000U) / elapsed) : 0U;
    osal_printk("[PANEL_OFFLINE] upload done bytes=%u packets=%u elapsed=%ums rate=%uB/s\r\n",
                (unsigned int)total_size, (unsigned int)packet_count,
                (unsigned int)elapsed, (unsigned int)rate);

    return ERRCODE_SUCC;
}

static void poll_execution_until_idle(void)
{
    uint32_t start = (uint32_t)uapi_systick_get_ms();
    bool saw_executing = false;

    while ((uint32_t)((uint32_t)uapi_systick_get_ms() - start) < PANEL_OFFLINE_EXEC_TIMEOUT_MS) {
        (void)send_packet_no_ack(PKT_STATUS_REQ, NULL, 0);
        osal_msleep(PANEL_OFFLINE_STATUS_MS);
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
    if (entry->size_bytes == 0U || entry->size_bytes > PANEL_OFFLINE_JOB_MAX_SIZE) {
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

    if (upload_selected_file(index, entry, crc) != ERRCODE_SUCC) {
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
    while (1) {
        panel_transport_sle_poll();
        if (!g_start_requested) {
            osal_msleep(50);
            continue;
        }

        g_start_requested = false;
        g_busy = true;
        run_offline_job(g_pending_index);
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
    if (mgr == NULL || mgr->selected_index < 0 || g_busy || g_start_requested) {
        return ERRCODE_FAIL;
    }
    g_pending_index = (uint8_t)mgr->selected_index;
    g_start_requested = true;
    osal_printk("[PANEL_OFFLINE] start request index=%u\r\n", (unsigned int)g_pending_index);
    return ERRCODE_SUCC;
}

bool panel_offline_job_is_busy(void)
{
    return g_busy || g_start_requested;
}
