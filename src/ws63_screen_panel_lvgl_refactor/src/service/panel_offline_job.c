/**
 * @file panel_offline_job.c
 * @brief Reads selected TF/SD G-code and sends it to RX using SLE Job packets.
 */
#include "panel_offline_job.h"
#include "panel_file_manager.h"
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

static osal_semaphore g_ack_sem;
static volatile bool g_ack_sem_ready;
static volatile bool g_wait_active;
static volatile bool g_wait_got_ack;
static volatile uint16_t g_wait_ack_seq;
static volatile uint8_t g_wait_status;
static uint16_t g_tx_seq = 1;
static volatile bool g_busy;
static volatile bool g_start_requested;
static uint8_t g_pending_index;

static uint16_t next_seq(void)
{
    uint16_t seq = g_tx_seq++;
    if (g_tx_seq == 0U) {
        g_tx_seq = 1U;
    }
    return seq;
}

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
    uint16_t seq = next_seq();
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

    while (g_ack_sem_ready && osal_sem_down_timeout(&g_ack_sem, 0) == OSAL_SUCCESS) {
    }

    g_wait_ack_seq = seq;
    g_wait_status = JOB_STATUS_INTERNAL_ERROR;
    g_wait_got_ack = false;
    g_wait_active = true;

    for (uint32_t retry = 0; retry <= PANEL_OFFLINE_RETRY_MAX; retry++) {
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
    uint16_t seq = next_seq();
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
        if (eof && offset < total_size) {
            return ERRCODE_FAIL;
        }
    }

    *out_crc = crc;
    return ERRCODE_SUCC;
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
    uint8_t buf[PANEL_OFFLINE_CHUNK_MAX];
    uint32_t offset = 0;
    uint32_t total_size = entry->size_bytes;

    errcode_t ret = send_job_begin(PANEL_OFFLINE_JOB_ID, total_size, crc);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    while (offset < total_size) {
        size_t bytes_read = 0;
        bool eof = false;
        if (!panel_file_manager_read_chunk(index, offset, buf, sizeof(buf), &bytes_read, &eof) ||
            bytes_read == 0U) {
            return ERRCODE_FAIL;
        }
        ret = send_job_data(PANEL_OFFLINE_JOB_ID, offset, buf, (uint16_t)bytes_read);
        if (ret != ERRCODE_SUCC) {
            return ret;
        }
        offset += (uint32_t)bytes_read;
        panel_model_offline_upload_progress(offset);
        if (eof && offset < total_size) {
            return ERRCODE_FAIL;
        }
    }

    return send_job_end(PANEL_OFFLINE_JOB_ID, total_size, crc);
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
    if (compute_file_crc(index, entry->size_bytes, &crc) != ERRCODE_SUCC) {
        panel_model_offline_error("READ_CRC_FAIL");
        return;
    }

    osal_printk("[PANEL_OFFLINE] begin file=%s size=%u crc=0x%04x\r\n",
                entry->name, (unsigned int)entry->size_bytes, crc);

    if (upload_selected_file(index, entry, crc) != ERRCODE_SUCC) {
        abort_rx_best_effort();
        panel_model_offline_error("UPLOAD_FAIL");
        return;
    }

    panel_model_offline_upload_progress(entry->size_bytes);
    if (send_exec_start(PANEL_OFFLINE_JOB_ID) != ERRCODE_SUCC) {
        abort_rx_best_effort();
        panel_model_offline_error("START_FAIL");
        return;
    }

    panel_model_offline_execution_started();
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
    return ERRCODE_SUCC;
}

bool panel_offline_job_is_busy(void)
{
    return g_busy || g_start_requested;
}
