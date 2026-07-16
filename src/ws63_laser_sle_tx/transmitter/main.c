/**
 * @file main.c
 * @brief TX board: UART job input to structured SLE packets.
 */
#include "app_init.h"
#include "common_def.h"
#include "config.h"
#include "crc16.h"
#include "errcode.h"
#include "packet.h"
#include "pinctrl.h"
#include "protocol.h"
#include "sle_errcode.h"
#include "sle_job_client.h"
#include "soc_osal.h"
#include "systick.h"
#include "uart.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TX_LINE_MAX 96
#define TX_PAYLOAD_BUF_SIZE SLE_JOB_PACKET_MAX_PAYLOAD
#define TX_DATA_RX_LOG_STEP 32U
#define TX_DATA_MODE_TIMEOUT_TICKS 5000U
#define TX_PANEL_STATUS_PERIOD_MS 200U
#define TX_PANEL_RX_STATUS_TIMEOUT_MS 1000U
#define TX_DATA_HOST_BUSY_PERIOD_MS 1000U
#define TX_FIRMWARE_PACKAGE "ws63-liteos-app_tx_all.fwpkg"

_Static_assert(sizeof(job_data_payload_t) + JOB_TX_DATA_CHUNK_MAX <= SLE_JOB_PACKET_MAX_PAYLOAD,
               "JOB_TX_DATA_CHUNK_MAX too large for SLE payload");
_Static_assert(JOB_TX_UART_QUEUE_SIZE > 1U &&
               (JOB_TX_UART_QUEUE_SIZE & (JOB_TX_UART_QUEUE_SIZE - 1U)) == 0U,
               "JOB_TX_UART_QUEUE_SIZE must be a power of two");

static uint8_t g_uart_rx_buf[JOB_TX_UART_RX_BUF_SIZE];
static uart_buffer_config_t g_uart_cfg = {
    .rx_buffer = g_uart_rx_buf,
    .rx_buffer_size = JOB_TX_UART_RX_BUF_SIZE,
};

static uint8_t g_uart_queue[JOB_TX_UART_QUEUE_SIZE];
static volatile uint32_t g_uart_queue_head = 0;
static volatile uint32_t g_uart_queue_tail = 0;
static volatile uint32_t g_uart_queue_drops = 0;
static volatile bool g_uart_queue_error = false;
static volatile bool g_uart_queue_discard = false;
static osal_wait g_uart_queue_wait;
static bool g_uart_queue_wait_ready = false;

static osal_semaphore g_ack_sem;
static bool g_ack_sem_ready = false;
static osal_semaphore g_status_sem;
static bool g_status_sem_ready = false;
static status_resp_payload_t g_last_status_resp;
static volatile bool g_last_status_resp_valid = false;
static volatile bool g_rx_status_req_pending = false;
static volatile bool g_rx_status_report_host = false;
static uint32_t g_rx_status_req_ms = 0;
static uint32_t g_rx_status_poll_ms = 0;
static uint8_t g_data_status_progress_bucket = 0;
static status_resp_payload_t g_panel_rx_status;
static volatile bool g_panel_rx_status_valid = false;
static volatile uint16_t g_wait_ack_seq = 0;
static volatile uint8_t g_wait_status = JOB_STATUS_INTERNAL_ERROR;
static volatile bool g_wait_got_ack = false;
static volatile bool g_wait_active = false;
static uint16_t g_tx_seq = 1;
static uint32_t g_diag_data_count = 0;
static uint32_t g_wait_start_ms = 0;

static uint32_t g_job_id = 0;
static uint32_t g_job_total = 0;
static uint32_t g_job_total_lines = 0;
static uint32_t g_job_offset = 0;
static uint16_t g_job_crc = 0;
static bool g_data_mode = false;
static uint32_t g_preroll_bytes = 0;
static bool g_preroll_signaled = false;
static bool g_rx_auto_start_enabled = false;
static uint8_t g_job_chunk[JOB_TX_DATA_CHUNK_MAX];
static uint16_t g_job_chunk_len = 0;
static uint32_t g_data_log_next = TX_DATA_RX_LOG_STEP;
static char g_line[TX_LINE_MAX];
static uint16_t g_line_len = 0;
static bool g_uart_control_frame_active = false;
static bool g_host_job_topology_active = false;
static volatile uint32_t g_async_data_ack_offset = 0;
static volatile uint16_t g_async_data_ack_seq = 0;
static volatile uint8_t g_async_data_status = JOB_STATUS_OK;
static volatile uint32_t g_async_data_credit = 0;
static volatile bool g_async_data_credit_valid = false;
static volatile bool g_async_data_error = false;
static volatile uint32_t g_async_data_last_ack_ms = 0;
static volatile uint32_t g_final_data_submit_ms = 0;
static volatile bool g_final_data_ack_logged = false;
static uint8_t g_async_retx_packet[SLE_JOB_PACKET_MAX_SIZE];
static uint16_t g_async_retx_packet_len = 0;
static uint16_t g_async_retx_seq = 0;
static uint32_t g_async_retx_data_index = 0;
static uint32_t g_async_retx_offset = 0;
static uint16_t g_async_retx_data_len = 0;
static uint32_t g_async_retx_next_offset = 0;
static uint32_t g_async_retx_last_send_ms = 0;
static uint32_t g_async_retx_count = 0;
static bool g_async_retx_valid = false;
static uint32_t g_panel_status_seq = 1;
static uint16_t g_panel_packet_seq = 1;
static uint32_t g_panel_status_last_ms = 0;
static volatile bool g_panel_status_pending = false;
static uint8_t g_panel_local_job_state = JOB_STATE_IDLE;
static bool g_panel_local_exec_started = false;
static uint32_t g_panel_local_last_error = JOB_STATUS_OK;
static bool g_panel_local_terminal_confirmed = false;

static void tx_panel_publish_local_status(bool force);
static void clear_async_data_retx(void);

static bool uart_queue_pop(uint8_t *ch)
{
    if (ch == NULL) {
        return false;
    }

    uint32_t lock = osal_irq_lock();
    uint32_t tail = g_uart_queue_tail;
    if (g_uart_queue_error || tail == g_uart_queue_head) {
        osal_irq_restore(lock);
        return false;
    }

    *ch = g_uart_queue[tail];
    g_uart_queue_tail = (tail + 1U) & (JOB_TX_UART_QUEUE_SIZE - 1U);
    osal_irq_restore(lock);
    return true;
}

static int uart_queue_ready(const void *param)
{
    unused(param);
    return g_uart_queue_error || g_uart_queue_head != g_uart_queue_tail;
}

static void uart_rx_callback(const void *buffer, uint16_t length, bool error)
{
    const uint8_t *data = (const uint8_t *)buffer;
    bool wake = false;

    if (error || (data == NULL && length > 0U)) {
        g_uart_queue_error = true;
        g_uart_queue_discard = true;
        wake = true;
    }

    if (g_uart_queue_discard) {
        g_uart_queue_drops += length;
    } else {
        wake = (g_uart_queue_head == g_uart_queue_tail);
        for (uint16_t i = 0; data != NULL && i < length; i++) {
            uint32_t head = g_uart_queue_head;
            uint32_t next = (head + 1U) & (JOB_TX_UART_QUEUE_SIZE - 1U);
            if (next == g_uart_queue_tail) {
                g_uart_queue_drops += (uint32_t)(length - i);
                g_uart_queue_error = true;
                g_uart_queue_discard = true;
                wake = true;
                break;
            }
            g_uart_queue[head] = data[i];
            g_uart_queue_head = next;
        }
    }

    if (wake && g_uart_queue_wait_ready) {
        osal_wait_wakeup(&g_uart_queue_wait);
    }
}

static void set_host_job_topology_active(bool active)
{
    if (g_host_job_topology_active == active) {
        return;
    }

    g_host_job_topology_active = active;
    if (active) {
        sle_job_client_set_background_seek_allowed(false);
    } else {
        sle_job_client_set_background_seek_allowed(true);
    }
    sle_job_client_poll_connect();
    osal_printk("[JOB_TX_TOPO] host_job=%u panel_link=%u seek_bg=%u\r\n",
                active ? 1U : 0U,
                sle_job_client_panel_link_allowed() ? 1U : 0U,
                active ? 0U : 1U);
}

static bool topology_state_is_terminal(uint8_t state)
{
    return state == JOB_STATE_IDLE ||
           state == JOB_STATE_ABORTED ||
           state == JOB_STATE_ERROR;
}

static void clear_local_job_state(void)
{
    g_job_id = 0;
    g_job_total = 0;
    g_job_total_lines = 0;
    g_job_offset = 0;
    g_job_crc = 0;
    g_data_mode = false;
    g_preroll_bytes = 0;
    g_preroll_signaled = false;
    g_rx_auto_start_enabled = false;
    g_job_chunk_len = 0;
    g_data_log_next = TX_DATA_RX_LOG_STEP;
    g_diag_data_count = 0;
    g_line_len = 0;
    g_uart_control_frame_active = false;
    g_async_data_ack_offset = 0;
    g_async_data_ack_seq = 0;
    g_async_data_status = JOB_STATUS_OK;
    g_async_data_credit = 0;
    g_async_data_credit_valid = false;
    g_async_data_error = false;
    g_async_data_last_ack_ms = 0;
    g_final_data_submit_ms = 0;
    g_final_data_ack_logged = false;
    clear_async_data_retx();
    g_panel_local_job_state = JOB_STATE_IDLE;
    g_panel_local_exec_started = false;
    g_panel_local_last_error = JOB_STATUS_OK;
    g_panel_local_terminal_confirmed = false;
    g_rx_status_req_pending = false;
    g_rx_status_report_host = false;
    g_rx_status_req_ms = 0;
    g_rx_status_poll_ms = 0;
    g_data_status_progress_bucket = 0;
    memset(&g_panel_rx_status, 0, sizeof(g_panel_rx_status));
    g_panel_rx_status_valid = false;
}

static uint16_t next_seq(void)
{
    uint16_t seq = g_tx_seq++;
    if (g_tx_seq == 0) {
        g_tx_seq = 1;
    }
    return seq;
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

static void clear_async_data_retx(void)
{
    g_async_retx_packet_len = 0;
    g_async_retx_seq = 0;
    g_async_retx_data_index = 0;
    g_async_retx_offset = 0;
    g_async_retx_data_len = 0;
    g_async_retx_next_offset = 0;
    g_async_retx_last_send_ms = 0;
    g_async_retx_count = 0;
    g_async_retx_valid = false;
}

static void note_async_data_ack_offset(uint32_t ack_offset)
{
    if (g_async_retx_valid && ack_offset >= g_async_retx_next_offset) {
        clear_async_data_retx();
    }
}

static void register_async_data_retx(const uint8_t *packet, uint16_t packet_len,
                                     uint16_t seq, uint32_t data_index,
                                     uint32_t offset, uint16_t data_len,
                                     uint32_t send_ms)
{
    uint32_t next_offset = offset + data_len;
    if (packet == NULL || packet_len == 0U || packet_len > sizeof(g_async_retx_packet) ||
        g_async_data_ack_offset >= next_offset) {
        clear_async_data_retx();
        return;
    }

    memcpy(g_async_retx_packet, packet, packet_len);
    g_async_retx_packet_len = packet_len;
    g_async_retx_seq = seq;
    g_async_retx_data_index = data_index;
    g_async_retx_offset = offset;
    g_async_retx_data_len = data_len;
    g_async_retx_next_offset = next_offset;
    g_async_retx_last_send_ms = send_ms;
    g_async_retx_count = 0;
    g_async_retx_valid = true;
}

static void maybe_retransmit_async_data(uint32_t waited)
{
    if (!g_async_retx_valid || g_async_retx_packet_len == 0U ||
        g_async_retx_count >= JOB_TX_DATA_RETX_MAX ||
        g_async_data_ack_offset >= g_async_retx_next_offset) {
        note_async_data_ack_offset(g_async_data_ack_offset);
        return;
    }

    uint32_t now = (uint32_t)uapi_systick_get_ms();
    uint32_t idle_ms = (g_async_retx_last_send_ms == 0U) ? waited :
                       (uint32_t)(now - g_async_retx_last_send_ms);
    if (idle_ms < JOB_TX_DATA_RETX_IDLE_MS) {
        return;
    }

    errcode_t ret = sle_job_client_send_packet_ex(g_async_retx_packet,
                                                  g_async_retx_packet_len,
                                                  false);
    g_async_retx_last_send_ms = now;
    if (ret == ERRCODE_SLE_SUCCESS) {
        g_async_retx_count++;
    }
    osal_printk("[TX_DATA_RETX] t=%u seq=%u data_idx=%u off=%u len=%u next=%u "
                "retx=%u/%u idle_ms=%u waited=%u ret=0x%x ack_off=%u ack_seq=%u "
                "link=%u client=%s\r\n",
                (unsigned int)now,
                (unsigned int)g_async_retx_seq,
                (unsigned int)g_async_retx_data_index,
                (unsigned int)g_async_retx_offset,
                (unsigned int)g_async_retx_data_len,
                (unsigned int)g_async_retx_next_offset,
                (unsigned int)g_async_retx_count,
                (unsigned int)JOB_TX_DATA_RETX_MAX,
                (unsigned int)idle_ms,
                (unsigned int)waited,
                (unsigned int)ret,
                (unsigned int)g_async_data_ack_offset,
                (unsigned int)g_async_data_ack_seq,
                (unsigned int)sle_job_client_is_connected(),
                sle_job_client_get_status());
}

static uint8_t tx_panel_current_job_state(void)
{
    if (g_panel_local_job_state != JOB_STATE_IDLE) {
        return g_panel_local_job_state;
    }
    if (g_data_mode) {
        return g_preroll_signaled ? JOB_STATE_EXECUTING : JOB_STATE_RECEIVING_JOB;
    }
    if (g_job_id != 0U && g_job_total != 0U) {
        return g_panel_local_exec_started ? JOB_STATE_EXECUTING : JOB_STATE_JOB_READY;
    }
    return JOB_STATE_IDLE;
}

static void tx_panel_mirror_status(const panel_status_payload_t *st)
{
    if (st == NULL || !sle_job_client_panel_is_connected()) {
        return;
    }

    uint8_t packet[SLE_JOB_PACKET_MAX_SIZE];
    uint16_t packet_len = 0;
    uint16_t packet_seq = g_panel_packet_seq++;
    if (g_panel_packet_seq == 0U) {
        g_panel_packet_seq = 1U;
    }
    if (sle_packet_encode(PKT_PANEL_STATUS, 0, packet_seq, st, sizeof(*st),
                          packet, sizeof(packet), &packet_len)) {
        (void)sle_job_client_mirror_panel_packet(packet, packet_len);
    }
}

static void tx_panel_publish_local_status(bool force)
{
    uint32_t now = (uint32_t)uapi_systick_get_ms();
    uint32_t elapsed = (g_panel_status_last_ms == 0U) ? 0xFFFFFFFFU :
                       (uint32_t)(now - g_panel_status_last_ms);

    if (force) {
        g_panel_status_pending = true;
    }

    uint32_t period_ms = TX_PANEL_STATUS_PERIOD_MS;
    bool event_due = g_panel_status_pending &&
                     (force || g_panel_status_last_ms == 0U ||
                      elapsed >= TX_PANEL_STATUS_PERIOD_MS);
    bool periodic_due = g_panel_status_last_ms == 0U || elapsed >= period_ms;
    if (!event_due && !periodic_due) {
        return;
    }

    bool has_job = (g_job_id != 0U && g_job_total != 0U) || g_host_job_topology_active ||
                   g_data_mode || tx_panel_current_job_state() != JOB_STATE_IDLE;
    bool use_rx_status = g_panel_rx_status_valid;
    if (use_rx_status && has_job && g_job_id != 0U) {
        use_rx_status = g_panel_rx_status.job_id == g_job_id &&
                        (g_panel_rx_status.total_size == 0U || g_job_total == 0U ||
                         g_panel_rx_status.total_size == g_job_total);
    }
    if (has_job && sle_job_client_is_connected() && !use_rx_status) {
        return;
    }

    panel_status_payload_t st = {0};
    st.seq = g_panel_status_seq++;
    if (g_panel_status_seq == 0U) {
        g_panel_status_seq = 1U;
    }
    if (use_rx_status) {
        const status_resp_payload_t *rx = &g_panel_rx_status;
        bool rx_has_job = rx->job_id != 0U || rx->total_size != 0U ||
                          rx->state != JOB_STATE_IDLE;
        st.owner = rx_has_job ? PANEL_OWNER_HOST : PANEL_OWNER_NONE;
        st.mode = (rx->status == JOB_STATUS_OK && rx->state != JOB_STATE_ERROR) ?
                  (rx_has_job ? PANEL_MODE_ONLINE : PANEL_MODE_IDLE) : PANEL_MODE_ERROR;
        st.job_state = rx->state;
        st.flags = rx_has_job ? PANEL_STATUS_FLAG_OWNER_LINK : 0U;
        if (sle_job_client_is_connected()) {
            st.flags |= PANEL_STATUS_FLAG_ANY_LINK;
        }
        if (topology_state_is_terminal(rx->state)) {
            st.flags |= PANEL_STATUS_FLAG_TERMINAL_CONFIRMED;
        }
        st.job_id = rx->job_id;
        st.received_size = rx->received_size;
        st.total_size = rx->total_size;
        st.executed_lines = rx->executed_lines;
        st.completed_lines = rx->completed_lines;
        st.total_lines = rx->total_lines;
        st.cache_free = rx->cache_free;
        st.last_error = rx->status;
    } else {
        st.owner = has_job ? PANEL_OWNER_HOST : PANEL_OWNER_NONE;
        st.mode = (g_panel_local_last_error == JOB_STATUS_OK) ?
                  (has_job ? PANEL_MODE_ONLINE : PANEL_MODE_IDLE) : PANEL_MODE_ERROR;
        st.job_state = has_job ? tx_panel_current_job_state() : JOB_STATE_IDLE;
        st.flags = has_job ? PANEL_STATUS_FLAG_OWNER_LINK : 0U;
        if (sle_job_client_is_connected()) {
            st.flags |= PANEL_STATUS_FLAG_ANY_LINK;
        }
        if (g_panel_local_terminal_confirmed) {
            st.flags |= PANEL_STATUS_FLAG_TERMINAL_CONFIRMED;
        }
        st.job_id = g_job_id;
        st.received_size = g_job_offset;
        st.total_size = g_job_total;
        st.executed_lines = 0;
        st.completed_lines = 0;
        st.total_lines = g_job_total_lines;
        st.cache_free = JOB_CACHE_SIZE - min_u32(g_job_offset, JOB_CACHE_SIZE);
        st.last_error = g_panel_local_last_error;
    }
    st.tick_ms = now;

    g_panel_status_last_ms = now;
    g_panel_status_pending = false;
    tx_panel_mirror_status(&st);
}

static void tx_panel_queue_after_rx_window(void)
{
    g_panel_status_pending = true;
}

static bool tx_panel_cache_rx_status(const status_resp_payload_t *rx)
{
    if (rx == NULL) {
        return false;
    }

    bool event_changed = !g_panel_rx_status_valid ||
                         g_panel_rx_status.job_id != rx->job_id ||
                         g_panel_rx_status.total_size != rx->total_size ||
                         g_panel_rx_status.state != rx->state ||
                         g_panel_rx_status.status != rx->status;
    memcpy(&g_panel_rx_status, rx, sizeof(g_panel_rx_status));
    g_panel_rx_status_valid = true;
    return event_changed;
}

static uint16_t peek_seq(void)
{
    return (g_tx_seq == 0) ? 1 : g_tx_seq;
}

static void tx_poll_rx_status_for_panel(void)
{
    uint32_t now = (uint32_t)uapi_systick_get_ms();
    bool active = g_host_job_topology_active || g_data_mode ||
                  g_panel_local_job_state != JOB_STATE_IDLE;
    if (!active || !sle_job_client_is_connected()) {
        return;
    }
    if (g_rx_status_req_pending) {
        if ((uint32_t)(now - g_rx_status_req_ms) < TX_PANEL_RX_STATUS_TIMEOUT_MS) {
            return;
        }
        g_rx_status_req_pending = false;
        g_rx_status_report_host = false;
    }
    if (g_rx_status_poll_ms != 0U &&
        (uint32_t)(now - g_rx_status_poll_ms) < TX_PANEL_STATUS_PERIOD_MS) {
        return;
    }
    while (g_status_sem_ready && osal_sem_down_timeout(&g_status_sem, 0) == OSAL_SUCCESS) {
    }
    g_last_status_resp_valid = false;

    uint8_t packet[SLE_JOB_PACKET_MAX_SIZE];
    uint16_t packet_len = 0;
    uint16_t seq = peek_seq();
    if (!sle_packet_encode(PKT_STATUS_REQ, 0, seq, NULL, 0, packet,
                           sizeof(packet), &packet_len)) {
        return;
    }
    if (sle_job_client_send_packet(packet, packet_len) == ERRCODE_SLE_SUCCESS) {
        g_rx_status_req_pending = true;
        g_rx_status_report_host = false;
        g_rx_status_req_ms = now;
        g_rx_status_poll_ms = now;
    }
}

static void host_sendf(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if (n >= (int)sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    (void)uapi_uart_write(LASER_UART_BUS, (const uint8_t *)buf, (uint32_t)n, 0);
}

static uint16_t payload_len_for_type(uint8_t type, const void *payload, uint16_t fallback)
{
    if (type == PKT_JOB_DATA && payload != NULL) {
        const job_data_payload_t *p = (const job_data_payload_t *)payload;
        return (uint16_t)(sizeof(job_data_payload_t) + p->data_len);
    }
    return fallback;
}

static bool tx_should_log_timing(uint8_t type, uint32_t data_index, uint32_t total_ms)
{
#if JOB_TX_TIMING_LOG
    if (type != PKT_JOB_DATA) {
        return true;
    }
    return data_index <= JOB_TX_TIMING_FIRST_PACKETS ||
           (JOB_TX_TIMING_EVERY_PACKETS > 0U && (data_index % JOB_TX_TIMING_EVERY_PACKETS) == 0U) ||
           total_ms >= JOB_TX_TIMING_SLOW_MS;
#else
    unused(type);
    unused(data_index);
    unused(total_ms);
    return false;
#endif
}

static uint32_t tx_ack_timeout_ms_for_type(uint8_t type)
{
    if (type == PKT_JOB_DATA) {
        return JOB_TX_DATA_ACK_TIMEOUT_MS;
    }
    if (type == PKT_EXEC_START) {
        return JOB_TX_EXEC_START_ACK_TIMEOUT_MS;
    }
    return JOB_TX_ACK_TIMEOUT_MS;
}

static uint32_t tx_async_data_window_bytes(void)
{
    uint32_t packets = g_rx_auto_start_enabled ?
                       JOB_TX_DATA_WINDOW_EXEC_PACKETS :
                       JOB_TX_DATA_WINDOW_UPLOAD_PACKETS;
    if (packets == 0U) {
        packets = 1U;
    }
    return packets * JOB_TX_DATA_CHUNK_MAX;
}

static bool tx_data_async_stream_enabled(void)
{
    return JOB_TX_DATA_ASYNC_AFTER_PREROLL &&
           (g_preroll_signaled ||
            JOB_TX_DATA_FAST_CUM_ACK_ENABLE);
}

static bool status_matches_active_data_job(const status_resp_payload_t *st)
{
    if (st == NULL || g_job_id == 0U || st->status != JOB_STATUS_OK ||
        st->job_id != g_job_id || st->total_size != g_job_total ||
        st->received_size > st->total_size) {
        return false;
    }

    return st->state == JOB_STATE_RECEIVING_JOB ||
           st->state == JOB_STATE_JOB_READY ||
           st->state == JOB_STATE_EXECUTING ||
           st->state == JOB_STATE_PAUSED;
}

static void update_async_data_progress_from_status(const status_resp_payload_t *st)
{
    if (!tx_data_async_stream_enabled() || !status_matches_active_data_job(st)) {
        return;
    }

    g_async_data_credit = st->cache_free;
    g_async_data_credit_valid = true;
    if (st->received_size > g_async_data_ack_offset) {
        uint32_t old_ack = g_async_data_ack_offset;
        g_async_data_ack_offset = st->received_size;
        note_async_data_ack_offset(g_async_data_ack_offset);
        uint32_t now = (uint32_t)uapi_systick_get_ms();
        bool final_progress = st->total_size > 0U && st->received_size >= st->total_size;
        uint8_t progress_bucket = (st->total_size > 0U) ?
                                  (uint8_t)(((uint64_t)st->received_size * 4U) /
                                            st->total_size) : 0U;
        if (final_progress || progress_bucket > g_data_status_progress_bucket) {
            g_data_status_progress_bucket = progress_bucket;
            osal_printk("[TX_DATA_STATUS_PROGRESS] t=%u job=%u rx=%u old_ack=%u total=%u "
                        "free=%u state=%u lines=%u\r\n",
                        (unsigned int)now,
                        (unsigned int)st->job_id,
                        (unsigned int)st->received_size,
                        (unsigned int)old_ack,
                        (unsigned int)st->total_size,
                        (unsigned int)st->cache_free,
                        (unsigned int)st->state,
                        (unsigned int)st->executed_lines);
        }
    }
}

#if JOB_TX_DATA_WINDOW_STATUS_PROBE_ENABLE
static bool request_rx_status_sync(const char *reason, uint16_t ref_seq,
                                   uint32_t ref_offset,
                                   status_resp_payload_t *out,
                                   uint32_t wait_ms)
{
    if (!g_status_sem_ready) {
        return false;
    }

    if (g_rx_status_req_pending) {
        if (osal_sem_down_timeout(&g_status_sem, wait_ms) == OSAL_SUCCESS &&
            g_last_status_resp_valid) {
            if (out != NULL) {
                memcpy(out, &g_last_status_resp, sizeof(*out));
            }
            return true;
        }
        g_rx_status_req_pending = false;
        g_rx_status_report_host = false;
    }

    while (osal_sem_down_timeout(&g_status_sem, 0) == OSAL_SUCCESS) {
    }
    g_last_status_resp_valid = false;

    uint8_t packet[SLE_JOB_PACKET_MAX_SIZE];
    uint16_t packet_len = 0;
    uint16_t seq = peek_seq();
    if (!sle_packet_encode(PKT_STATUS_REQ, 0, seq, NULL, 0, packet,
                           sizeof(packet), &packet_len)) {
        return false;
    }

    uint32_t t_send = (uint32_t)uapi_systick_get_ms();
    g_rx_status_req_pending = true;
    g_rx_status_report_host = false;
    g_rx_status_req_ms = t_send;
    errcode_t ret = sle_job_client_send_packet(packet, packet_len);
    uint32_t send_ms = (uint32_t)uapi_systick_get_ms() - t_send;
    osal_printk("[TX_STATUS_PROBE] t=%u reason=%s status_seq=%u ref_seq=%u "
                "ref_off=%u ret=0x%x send_ms=%u ack_off=%u ack_seq=%u "
                "seq_neutral=1\r\n",
                (unsigned int)uapi_systick_get_ms(),
                (reason != NULL) ? reason : "none",
                (unsigned int)seq, (unsigned int)ref_seq,
                (unsigned int)ref_offset, (unsigned int)ret,
                (unsigned int)send_ms,
                (unsigned int)g_async_data_ack_offset,
                (unsigned int)g_async_data_ack_seq);
    if (ret != ERRCODE_SLE_SUCCESS) {
        g_rx_status_req_pending = false;
        return false;
    }

    if (osal_sem_down_timeout(&g_status_sem, wait_ms) == OSAL_SUCCESS &&
        g_last_status_resp_valid) {
        if (out != NULL) {
            memcpy(out, &g_last_status_resp, sizeof(*out));
        }
        return true;
    }

    osal_printk("[TX_STATUS_PROBE_TIMEOUT] t=%u reason=%s status_seq=%u "
                "ref_seq=%u ref_off=%u wait_ms=%u\r\n",
                (unsigned int)uapi_systick_get_ms(),
                (reason != NULL) ? reason : "none",
                (unsigned int)seq, (unsigned int)ref_seq,
                (unsigned int)ref_offset, (unsigned int)wait_ms);
    return false;
}

static bool request_rx_status_probe(uint16_t data_seq, uint32_t data_index,
                                    uint32_t offset, uint16_t len,
                                    uint32_t waited)
{
    status_resp_payload_t st = {0};
    bool ok = request_rx_status_sync("data_window", data_seq, offset, &st,
                                     JOB_TX_DATA_STATUS_PROBE_WAIT_MS);
    osal_printk("[TX_DATA_STATUS_PROBE] t=%u status_seq=%u data_seq=%u data_idx=%u "
                "off=%u len=%u waited=%u ok=%u status_rx=%u status_total=%u "
                "status_state=%u ack_off=%u ack_seq=%u\r\n",
                (unsigned int)uapi_systick_get_ms(), (unsigned int)peek_seq(), data_seq,
                (unsigned int)data_index, (unsigned int)offset,
                (unsigned int)len, (unsigned int)waited,
                (unsigned int)(ok ? 1U : 0U),
                (unsigned int)(ok ? st.received_size : 0U),
                (unsigned int)(ok ? st.total_size : 0U),
                (unsigned int)(ok ? st.state : 0U),
                (unsigned int)g_async_data_ack_offset,
                (unsigned int)g_async_data_ack_seq);
    return ok;
}
#endif

static bool wait_async_data_window(uint16_t seq, uint32_t data_index,
                                   uint32_t offset, uint16_t len)
{
    uint32_t next_offset = offset + len;
    uint32_t window = tx_async_data_window_bytes();
    uint32_t start = (uint32_t)uapi_systick_get_ms();
    uint32_t last_host_busy_ms = 0;
    uint32_t last_probe_waited_ms = 0;
    uint32_t last_credit_probe_ms = 0;
    bool logged_stall = false;

    while (!g_async_data_error) {
        uint32_t ack_offset = g_async_data_ack_offset;
        uint32_t outstanding = (next_offset > ack_offset) ? (next_offset - ack_offset) : 0U;
        bool window_allowed = outstanding <= window;
        bool credit_allowed = !g_async_data_credit_valid || outstanding <= g_async_data_credit;
        if (window_allowed && credit_allowed) {
            return true;
        }

        uint32_t waited = (uint32_t)uapi_systick_get_ms() - start;
        if (waited >= TX_DATA_HOST_BUSY_PERIOD_MS &&
            (last_host_busy_ms == 0U ||
             (uint32_t)(waited - last_host_busy_ms) >= TX_DATA_HOST_BUSY_PERIOD_MS)) {
            last_host_busy_ms = waited;
            host_sendf("@BUSY type=%u offset=%u ack_offset=%u outstanding=%u window=%u waited=%u\r\n",
                       PKT_JOB_DATA, (unsigned int)next_offset,
                       (unsigned int)ack_offset, (unsigned int)outstanding,
                       (unsigned int)window, (unsigned int)waited);
        }
        if (!window_allowed) {
            maybe_retransmit_async_data(waited);
        }
#if JOB_TX_DATA_WINDOW_STATUS_PROBE_ENABLE
        if (window_allowed && !credit_allowed &&
            (last_credit_probe_ms == 0U ||
             (uint32_t)(waited - last_credit_probe_ms) >= JOB_TX_DATA_CREDIT_PROBE_INTERVAL_MS)) {
            last_credit_probe_ms = waited;
            status_resp_payload_t st = {0};
            (void)request_rx_status_sync("data_credit", seq, offset, &st,
                                         JOB_TX_DATA_STATUS_PROBE_WAIT_MS);
            continue;
        }
#endif
        if (waited >= JOB_TX_DATA_WINDOW_STALL_MS && !logged_stall) {
            logged_stall = true;
            osal_printk("[TX_DATA_WIN_STALL_OBSERVE] t=%u seq=%u data_idx=%u off=%u len=%u "
                        "next=%u ack_off=%u ack_seq=%u outstanding=%u window=%u "
                        "status=%u credit=%u waited=%u probe=%u timeout=%u link=%u client=%s\r\n",
                        (unsigned int)uapi_systick_get_ms(), seq,
                        (unsigned int)data_index, (unsigned int)offset,
                        (unsigned int)len, (unsigned int)next_offset,
                        (unsigned int)ack_offset, (unsigned int)g_async_data_ack_seq,
                        (unsigned int)outstanding, (unsigned int)window,
                        (unsigned int)g_async_data_status,
                        (unsigned int)g_async_data_credit,
                        (unsigned int)waited,
                        (unsigned int)JOB_TX_DATA_WINDOW_STATUS_PROBE_ENABLE,
                        (unsigned int)JOB_TX_ASYNC_DATA_DRAIN_TIMEOUT_MS,
                        (unsigned int)sle_job_client_is_connected(),
                        sle_job_client_get_status());
        }

#if JOB_TX_DATA_WINDOW_STATUS_PROBE_ENABLE
        if (waited >= JOB_TX_DATA_WINDOW_STALL_MS &&
            (last_probe_waited_ms == 0U ||
             (uint32_t)(waited - last_probe_waited_ms) >= JOB_TX_DATA_WINDOW_STALL_MS)) {
            last_probe_waited_ms = waited;
            bool probed = request_rx_status_probe(seq, data_index, offset, len, waited);
            ack_offset = g_async_data_ack_offset;
            outstanding = (next_offset > ack_offset) ? (next_offset - ack_offset) : 0U;
            if (outstanding <= window) {
                osal_printk("[TX_DATA_WIN_PROBE_OK] t=%u seq=%u data_idx=%u off=%u len=%u "
                            "next=%u ack_off=%u ack_seq=%u outstanding=%u window=%u "
                            "credit=%u waited=%u probed=%u\r\n",
                            (unsigned int)uapi_systick_get_ms(), seq,
                            (unsigned int)data_index, (unsigned int)offset,
                            (unsigned int)len, (unsigned int)next_offset,
                            (unsigned int)ack_offset,
                            (unsigned int)g_async_data_ack_seq,
                            (unsigned int)outstanding, (unsigned int)window,
                            (unsigned int)g_async_data_credit,
                            (unsigned int)waited, (unsigned int)(probed ? 1U : 0U));
                return true;
            }
            osal_printk("[TX_DATA_WIN_STALL] t=%u seq=%u data_idx=%u off=%u len=%u "
                        "next=%u ack_off=%u ack_seq=%u outstanding=%u window=%u "
                        "status=%u credit=%u waited=%u probed=%u link=%u client=%s\r\n",
                        (unsigned int)uapi_systick_get_ms(), seq,
                        (unsigned int)data_index, (unsigned int)offset,
                        (unsigned int)len, (unsigned int)next_offset,
                        (unsigned int)ack_offset, (unsigned int)g_async_data_ack_seq,
                        (unsigned int)outstanding, (unsigned int)window,
                        (unsigned int)g_async_data_status,
                        (unsigned int)g_async_data_credit,
                        (unsigned int)waited,
                        (unsigned int)(probed ? 1U : 0U),
                        (unsigned int)sle_job_client_is_connected(),
                        sle_job_client_get_status());
        }
        if (waited >= JOB_TX_ASYNC_DATA_DRAIN_TIMEOUT_MS) {
            osal_printk("[TX_DATA_WIN_STALL_TIMEOUT] t=%u seq=%u data_idx=%u off=%u len=%u "
                        "next=%u ack_off=%u ack_seq=%u outstanding=%u window=%u "
                        "status=%u credit=%u waited=%u probe=1 link=%u client=%s\r\n",
                        (unsigned int)uapi_systick_get_ms(), seq,
                        (unsigned int)data_index, (unsigned int)offset,
                        (unsigned int)len, (unsigned int)next_offset,
                        (unsigned int)ack_offset, (unsigned int)g_async_data_ack_seq,
                        (unsigned int)outstanding, (unsigned int)window,
                        (unsigned int)g_async_data_status,
                        (unsigned int)g_async_data_credit,
                        (unsigned int)waited,
                        (unsigned int)sle_job_client_is_connected(),
                        sle_job_client_get_status());
            return false;
        }
#else
        if (waited >= JOB_TX_ASYNC_DATA_DRAIN_TIMEOUT_MS) {
            osal_printk("[TX_DATA_WIN_STALL_TIMEOUT] t=%u seq=%u data_idx=%u off=%u len=%u "
                        "next=%u ack_off=%u ack_seq=%u outstanding=%u window=%u "
                        "status=%u credit=%u waited=%u probe=0 link=%u client=%s\r\n",
                        (unsigned int)uapi_systick_get_ms(), seq,
                        (unsigned int)data_index, (unsigned int)offset,
                        (unsigned int)len, (unsigned int)next_offset,
                        (unsigned int)ack_offset, (unsigned int)g_async_data_ack_seq,
                        (unsigned int)outstanding, (unsigned int)window,
                        (unsigned int)g_async_data_status,
                        (unsigned int)g_async_data_credit,
                        (unsigned int)waited,
                        (unsigned int)sle_job_client_is_connected(),
                        sle_job_client_get_status());
            return false;
        }
#endif

        osal_msleep(JOB_TX_DATA_WINDOW_POLL_MS);
    }

    osal_printk("[TX_DATA_WIN_ERR] t=%u seq=%u data_idx=%u off=%u len=%u "
                "ack_off=%u ack_seq=%u status=%u credit=%u\r\n",
                (unsigned int)uapi_systick_get_ms(), seq,
                (unsigned int)data_index, (unsigned int)offset,
                (unsigned int)len, (unsigned int)g_async_data_ack_offset,
                (unsigned int)g_async_data_ack_seq,
                (unsigned int)g_async_data_status,
                (unsigned int)g_async_data_credit);
    return false;
}

static bool handle_async_data_ack(const ack_payload_t *ack)
{
    if (ack == NULL || ack->ack_type != PKT_JOB_DATA) {
        return false;
    }
    if (g_wait_active && ack->ack_seq == g_wait_ack_seq) {
        return false;
    }

    uint32_t now_ms = (uint32_t)uapi_systick_get_ms();
    uint32_t old_ack_offset = g_async_data_ack_offset;
    uint32_t last_ack_ms = g_async_data_last_ack_ms;
    uint32_t ack_gap_ms = (last_ack_ms == 0U) ? 0U : (uint32_t)(now_ms - last_ack_ms);

    if (ack->status != JOB_STATUS_OK) {
        g_async_data_ack_offset = ack->offset;
        g_async_data_ack_seq = ack->ack_seq;
        g_async_data_status = ack->status;
        g_async_data_credit = ack->credit;
        g_async_data_credit_valid = true;
        g_async_data_error = true;
        g_async_data_last_ack_ms = now_ms;
        clear_async_data_retx();
        osal_printk("[TX_DATA_ASYNC_NACK] t=%u seq=%u status=%u off=%u credit=%u wait=%u active=%u\r\n",
                    (unsigned int)now_ms, ack->ack_seq, ack->status,
                    (unsigned int)ack->offset, (unsigned int)ack->credit,
                    (unsigned int)g_wait_ack_seq, (unsigned int)g_wait_active);
        host_sendf("@NACK type=%u seq=%u status=%u offset=%u async=1\r\n",
                   PKT_JOB_DATA, ack->ack_seq, ack->status, (unsigned int)ack->offset);
        return true;
    }

    if (ack->offset < g_async_data_ack_offset) {
        osal_printk("[TX_DATA_ASYNC_OLD_ACK] t=%u seq=%u off=%u current_off=%u credit=%u\r\n",
                    (unsigned int)uapi_systick_get_ms(), ack->ack_seq,
                    (unsigned int)ack->offset,
                    (unsigned int)g_async_data_ack_offset,
                    (unsigned int)ack->credit);
        return true;
    }

    g_async_data_ack_offset = ack->offset;
    g_async_data_ack_seq = ack->ack_seq;
    g_async_data_status = ack->status;
    g_async_data_credit = ack->credit;
    g_async_data_credit_valid = true;
    g_async_data_last_ack_ms = now_ms;
    note_async_data_ack_offset(g_async_data_ack_offset);

    if (!g_final_data_ack_logged && g_final_data_submit_ms != 0U &&
        g_job_total > 0U && ack->offset >= g_job_total) {
        g_final_data_ack_logged = true;
        osal_printk("[TX_FINAL_ACK_RX] seq=%u off=%u submit_to_ack_ms=%u\r\n",
                    (unsigned int)ack->ack_seq,
                    (unsigned int)ack->offset,
                    (unsigned int)(now_ms - g_final_data_submit_ms));
    }

    if ((ack_gap_ms >= JOB_TX_TIMING_SLOW_MS) ||
        (JOB_TX_TIMING_EVERY_PACKETS > 0U &&
         ((uint32_t)ack->ack_seq % JOB_TX_TIMING_EVERY_PACKETS) == 0U)) {
        uint32_t delta = (ack->offset >= old_ack_offset) ?
                         (ack->offset - old_ack_offset) : 0U;
        osal_printk("[TX_DATA_ASYNC_ACK] t=%u seq=%u off=%u old_off=%u delta=%u "
                    "gap_ms=%u credit=%u wait_seq=%u active=%u\r\n",
                    (unsigned int)now_ms, ack->ack_seq,
                    (unsigned int)ack->offset,
                    (unsigned int)old_ack_offset,
                    (unsigned int)delta,
                    (unsigned int)ack_gap_ms,
                    (unsigned int)ack->credit,
                    (unsigned int)g_wait_ack_seq,
                    (unsigned int)g_wait_active);
    }
    return true;
}

static errcode_t send_packet_wait_ack_internal(uint8_t type, const void *payload,
                                               uint16_t payload_len,
                                               bool report_host_failure)
{
    uint8_t packet[SLE_JOB_PACKET_MAX_SIZE];
    uint16_t packet_len = 0;
    uint16_t seq = next_seq();
    uint16_t actual_payload_len = payload_len_for_type(type, payload, payload_len);
    uint32_t dbg_off = 0;
    uint16_t dbg_dlen = 0;
    uint32_t dbg_data_index = 0;

    if (!sle_packet_encode(type, 0, seq, payload, actual_payload_len,
                           packet, sizeof(packet), &packet_len)) {
        osal_printk("[JOB_TX] encode fail type=0x%02x len=%u\r\n", type, actual_payload_len);
        return ERRCODE_FAIL;
    }

    while (g_ack_sem_ready && osal_sem_down_timeout(&g_ack_sem, 0) == OSAL_SUCCESS) {
    }
    g_wait_ack_seq = seq;
    g_wait_status = JOB_STATUS_INTERNAL_ERROR;
    g_wait_got_ack = false;
    g_wait_active = true;

    {
        if (type == PKT_JOB_DATA && payload != NULL && payload_len >= sizeof(job_data_payload_t)) {
            const job_data_payload_t *dp = (const job_data_payload_t *)payload;
            dbg_off = dp->offset;
            dbg_dlen = dp->data_len;
            g_diag_data_count++;
            dbg_data_index = g_diag_data_count;
        }
        if (JOB_DIAG_LOG && g_diag_data_count <= JOB_DIAG_LOG_MAX_DATA) {
            osal_printk("[TX_SEND] t=%u seq=%u type=0x%02x off=%u dlen=%u plen=%u\r\n",
                        (unsigned int)uapi_systick_get_ms(), seq, type,
                        (unsigned int)dbg_off, (unsigned int)dbg_dlen,
                        (unsigned int)actual_payload_len);
        }
    }

    if (JOB_DIAG_LOG) {
        osal_printk("[JOB_TX_SEND_ENTER] type=0x%02x seq=%u payload=%u packet=%u conn=%u status=%s\r\n",
                    type, seq, actual_payload_len, packet_len,
                    (unsigned int)sle_job_client_is_connected(), sle_job_client_get_status());
    }

    uint32_t ack_timeout_ms = tx_ack_timeout_ms_for_type(type);
    bool force_write_req = (type == PKT_JOB_DATA && g_preroll_signaled &&
                            JOB_TX_DATA_FORCE_REQ_AFTER_PREROLL);

    for (uint32_t retry = 0; retry <= JOB_TX_RETRY_MAX; retry++) {
        uint32_t reconnect_start = (uint32_t)uapi_systick_get_ms();
        while (!sle_job_client_is_connected()) {
            uint32_t reconnect_elapsed =
                (uint32_t)uapi_systick_get_ms() - reconnect_start;
            if (reconnect_elapsed >= JOB_TX_CONNECT_WAIT_MS) {
                osal_printk("[JOB_TX_WAIT_CONN_TIMEOUT] type=0x%02x seq=%u waited=%u status=%s\r\n",
                            type, seq, (unsigned int)reconnect_elapsed,
                            sle_job_client_get_status());
                g_wait_active = false;
                g_wait_ack_seq = 0;
                if (report_host_failure) {
                    host_sendf("@NACK type=%u seq=%u status=%u reason=no_link\r\n",
                               type, seq, JOB_STATUS_NOT_READY);
                }
                return ERRCODE_FAIL;
            }
            sle_job_client_poll_connect();
            osal_msleep(20);
        }

        if (g_wait_got_ack && g_wait_status == JOB_STATUS_OK) {
            if (retry > 0U) {
                osal_printk("[TX_LATE_ACK_OK] t=%u type=0x%02x seq=%u data_idx=%u off=%u len=%u "
                            "retry=%u wait_cost=%u timeout=%u force_req=%u status=%u link=%u client=%s\r\n",
                            (unsigned int)uapi_systick_get_ms(), type, seq,
                            (unsigned int)dbg_data_index, (unsigned int)dbg_off,
                            (unsigned int)dbg_dlen, (unsigned int)retry,
                            (unsigned int)(uapi_systick_get_ms() - g_wait_start_ms),
                            (unsigned int)ack_timeout_ms,
                            (unsigned int)(force_write_req ? 1U : 0U),
                            (unsigned int)g_wait_status,
                            (unsigned int)sle_job_client_is_connected(),
                            sle_job_client_get_status());
            }
            g_wait_active = false;
            g_wait_ack_seq = 0;
            return ERRCODE_SUCC;
        }

        if (JOB_DIAG_LOG) {
            osal_printk("[JOB_TX_SEND_CALL] type=0x%02x seq=%u try=%u packet=%u status=%s\r\n",
                        type, seq, (unsigned int)retry, packet_len, sle_job_client_get_status());
        }
        uint32_t t_send = (uint32_t)uapi_systick_get_ms();
        errcode_t ret = sle_job_client_send_packet_ex(packet, packet_len, force_write_req);
        uint32_t send_ms = (uint32_t)uapi_systick_get_ms() - t_send;
        uint32_t wait_ms = 0;
        if (JOB_DIAG_LOG && (g_diag_data_count <= JOB_DIAG_LOG_MAX_DATA || ret != ERRCODE_SLE_SUCCESS)) {
            osal_printk("[TX_CFM] t=%u seq=%u ret=0x%x cost=%u\r\n",
                        (unsigned int)uapi_systick_get_ms(), seq,
                        (unsigned int)ret,
                        (unsigned int)send_ms);
        }
        if (JOB_DIAG_LOG) {
            osal_printk("[JOB_TX_FRAME] type=0x%02x seq=%u len=%u try=%u ret=0x%x\r\n",
                        type, seq, packet_len, (unsigned int)retry, ret);
        }
        if (ret == ERRCODE_SLE_SUCCESS) {
            uint32_t t_ack = (uint32_t)uapi_systick_get_ms();
            g_wait_start_ms = t_ack;
            if (osal_sem_down_timeout(&g_ack_sem, ack_timeout_ms) == OSAL_SUCCESS &&
                g_wait_got_ack) {
                uint32_t ack_ms = (uint32_t)uapi_systick_get_ms() - t_ack;
                uint32_t total_ms = (uint32_t)uapi_systick_get_ms() - t_send;
                if (tx_should_log_timing(type, dbg_data_index, total_ms)) {
                    osal_printk("[TX_TIMING] type=0x%02x seq=%u data_idx=%u off=%u len=%u "
                                "send_ms=%u ack_ms=%u total_ms=%u retry=%u force_req=%u status=%u link=%u client=%s\r\n",
                                type, seq, (unsigned int)dbg_data_index,
                                (unsigned int)dbg_off, (unsigned int)dbg_dlen,
                                (unsigned int)send_ms, (unsigned int)ack_ms,
                                (unsigned int)total_ms, (unsigned int)retry,
                                (unsigned int)(force_write_req ? 1U : 0U),
                                (unsigned int)g_wait_status,
                                (unsigned int)sle_job_client_is_connected(),
                                sle_job_client_get_status());
                }
                if (g_wait_status == JOB_STATUS_OK) {
                    if (retry > 0U) {
                        osal_printk("[TX_RETRY_OK] t=%u type=0x%02x seq=%u data_idx=%u off=%u len=%u "
                                    "retry=%u send_ms=%u ack_ms=%u total_ms=%u timeout=%u force_req=%u link=%u client=%s\r\n",
                                    (unsigned int)uapi_systick_get_ms(), type, seq,
                                    (unsigned int)dbg_data_index, (unsigned int)dbg_off,
                                    (unsigned int)dbg_dlen, (unsigned int)retry,
                                    (unsigned int)send_ms, (unsigned int)ack_ms,
                                    (unsigned int)total_ms, (unsigned int)ack_timeout_ms,
                                    (unsigned int)(force_write_req ? 1U : 0U),
                                    (unsigned int)sle_job_client_is_connected(),
                                    sle_job_client_get_status());
                    }
                    g_wait_active = false;
                    g_wait_ack_seq = 0;
                    return ERRCODE_SUCC;
                }
                break;
            }
            wait_ms = (uint32_t)uapi_systick_get_ms() - t_ack;
            osal_printk("[TX_TO] t=%u type=0x%02x seq=%u data_idx=%u off=%u len=%u "
                        "retry=%u waited=%u timeout=%u force_req=%u send_ret=0x%x send_ms=%u got=%u st=%u link=%u status=%s\r\n",
                        (unsigned int)uapi_systick_get_ms(), type, seq,
                        (unsigned int)dbg_data_index, (unsigned int)dbg_off,
                        (unsigned int)dbg_dlen, (unsigned int)retry,
                        (unsigned int)wait_ms, (unsigned int)ack_timeout_ms,
                        (unsigned int)(force_write_req ? 1U : 0U), (unsigned int)ret,
                        (unsigned int)send_ms, (unsigned int)g_wait_got_ack,
                        (unsigned int)g_wait_status,
                        (unsigned int)sle_job_client_is_connected(),
                        sle_job_client_get_status());
        } else {
            osal_printk("[TX_SEND_FAIL] t=%u type=0x%02x seq=%u data_idx=%u off=%u len=%u "
                        "retry=%u force_req=%u send_ret=0x%x send_ms=%u link=%u status=%s\r\n",
                        (unsigned int)uapi_systick_get_ms(), type, seq,
                        (unsigned int)dbg_data_index, (unsigned int)dbg_off,
                        (unsigned int)dbg_dlen, (unsigned int)retry,
                        (unsigned int)(force_write_req ? 1U : 0U),
                        (unsigned int)ret, (unsigned int)send_ms,
                        (unsigned int)sle_job_client_is_connected(),
                        sle_job_client_get_status());
        }
    }

    osal_printk("[TX_FAIL] t=%u type=0x%02x seq=%u data_idx=%u off=%u len=%u "
                "status=%u got=%u retries=%u force_req=%u link=%u client=%s\r\n",
                (unsigned int)uapi_systick_get_ms(), type, seq,
                (unsigned int)dbg_data_index, (unsigned int)dbg_off,
                (unsigned int)dbg_dlen, (unsigned int)g_wait_status,
                (unsigned int)g_wait_got_ack, (unsigned int)JOB_TX_RETRY_MAX,
                (unsigned int)(force_write_req ? 1U : 0U),
                (unsigned int)sle_job_client_is_connected(),
                sle_job_client_get_status());
    g_wait_active = false;
    g_wait_ack_seq = 0;
    if (report_host_failure) {
        host_sendf("@NACK type=%u seq=%u status=%u\r\n", type, seq, g_wait_status);
    }
    return ERRCODE_FAIL;
}

static errcode_t send_packet_wait_ack(uint8_t type, const void *payload, uint16_t payload_len)
{
    return send_packet_wait_ack_internal(type, payload, payload_len, true);
}

static errcode_t abort_rx_and_clear_transaction(const char *reason)
{
    uint32_t old_job = g_job_id;
    uint32_t old_offset = g_job_offset;
    uint32_t old_total = g_job_total;
    bool was_data_mode = g_data_mode;

    /* Stop accepting bytes for the stale transaction before waiting on SLE. */
    clear_local_job_state();
    unused(reason);
    unused(old_job);
    unused(old_offset);
    unused(old_total);
    unused(was_data_mode);

    if (!sle_job_client_is_connected()) {
        osal_printk("[JOB_TX_RESYNC] RX abort not confirmed: SLE disconnected\r\n");
        set_host_job_topology_active(false);
        tx_panel_publish_local_status(true);
        return ERRCODE_FAIL;
    }

    errcode_t ret = send_packet_wait_ack(PKT_JOB_ABORT, NULL, 0);
    if (ret == ERRCODE_SUCC) {
        g_panel_local_terminal_confirmed = true;
    }
    set_host_job_topology_active(false);
    tx_panel_publish_local_status(true);
    return ret;
}

static void handle_uart_resync(void)
{
    if (abort_rx_and_clear_transaction("uart-can") == ERRCODE_SUCC) {
        host_sendf("@OK resync rx=aborted\r\n");
    } else {
        host_sendf("@ERR resync_failed rx=unconfirmed\r\n");
    }
}

static void response_cb(const uint8_t *data, uint16_t length)
{
    sle_packet_view_t pkt;
    if (!sle_packet_decode(data, length, &pkt)) {
        osal_printk("[JOB_TX] bad response len=%u\r\n", length);
        return;
    }

    if ((pkt.type == PKT_ACK || pkt.type == PKT_NACK) && pkt.len == sizeof(ack_payload_t)) {
        ack_payload_t ack;
        memcpy(&ack, pkt.payload, sizeof(ack));
        if (handle_async_data_ack(&ack)) {
            return;
        }
        if (!g_wait_active || ack.ack_seq != g_wait_ack_seq) {
            osal_printk("[TX_OLD] t=%u pkt_type=0x%02x ack_type=0x%02x ack_seq=%u wait=%u "
                        "st=%u off=%u credit=%u active=%u cost=%u\r\n",
                        (unsigned int)uapi_systick_get_ms(), pkt.type, ack.ack_type,
                        ack.ack_seq, (unsigned int)g_wait_ack_seq, ack.status,
                        (unsigned int)ack.offset, (unsigned int)ack.credit,
                        (unsigned int)g_wait_active,
                        (unsigned int)(uapi_systick_get_ms() - g_wait_start_ms));
            return;
        }
        if (JOB_DIAG_LOG && (g_diag_data_count <= JOB_DIAG_LOG_MAX_DATA || ack.status != 0)) {
            osal_printk("[TX_ACK] t=%u ack_seq=%u wait=%u st=%u off=%u active=%u cost=%u\r\n",
                        (unsigned int)uapi_systick_get_ms(), ack.ack_seq,
                        (unsigned int)g_wait_ack_seq, ack.status, (unsigned int)ack.offset,
                        (unsigned int)g_wait_active,
                        (unsigned int)(uapi_systick_get_ms() - g_wait_start_ms));
        }
        uint32_t ack_cb_ms = (uint32_t)uapi_systick_get_ms();
        uint32_t wait_cost_ms = (g_wait_start_ms == 0U) ? 0U :
                                (uint32_t)(ack_cb_ms - g_wait_start_ms);
        g_wait_status = ack.status;
        g_wait_got_ack = true;
        uint32_t sem_ms = 0;
        if (g_ack_sem_ready) {
            uint32_t sem_start_ms = (uint32_t)uapi_systick_get_ms();
            osal_sem_up(&g_ack_sem);
            sem_ms = (uint32_t)uapi_systick_get_ms() - sem_start_ms;
        }
        if (ack.ack_type != PKT_JOB_DATA || ack.status != JOB_STATUS_OK ||
            wait_cost_ms >= JOB_TX_TIMING_SLOW_MS || sem_ms > 0U) {
            osal_printk("[TX_ACK_TRACE] t=%u ack_type=0x%02x ack_seq=%u wait_seq=%u st=%u "
                        "off=%u credit=%u active=%u wait_ms=%u sem_ms=%u\r\n",
                        (unsigned int)ack_cb_ms,
                        (unsigned int)ack.ack_type,
                        (unsigned int)ack.ack_seq,
                        (unsigned int)g_wait_ack_seq,
                        (unsigned int)ack.status,
                        (unsigned int)ack.offset,
                        (unsigned int)ack.credit,
                        (unsigned int)g_wait_active,
                        (unsigned int)wait_cost_ms,
                        (unsigned int)sem_ms);
        }
        return;
    }

    if (pkt.type == PKT_STATUS_RESP && pkt.len == sizeof(status_resp_payload_t)) {
        status_resp_payload_t st = {0};
        memcpy(&st, pkt.payload, sizeof(st));
        bool report_host = g_rx_status_report_host;
        g_rx_status_req_pending = false;
        g_rx_status_report_host = false;
        memcpy(&g_last_status_resp, &st, sizeof(g_last_status_resp));
        g_last_status_resp_valid = true;
        update_async_data_progress_from_status(&st);
        bool panel_event_changed = tx_panel_cache_rx_status(&st);
        tx_panel_publish_local_status(panel_event_changed);
        if (g_status_sem_ready) {
            osal_sem_up(&g_status_sem);
        }
        if (JOB_DIAG_LOG) {
            osal_printk("[JOB_TX_STATUS] state=%u status=%u job=%u rx=%u/%u free=%u lines=%u\r\n",
                        st.state, st.status, (unsigned int)st.job_id,
                        (unsigned int)st.received_size, (unsigned int)st.total_size,
                        (unsigned int)st.cache_free, (unsigned int)st.executed_lines);
        }
        if (report_host) {
            host_sendf("@STATUS state=%u status=%u job=%u rx=%u total=%u free=%u lines=%u completed=%u total_lines=%u\r\n",
                       st.state, st.status, (unsigned int)st.job_id,
                       (unsigned int)st.received_size, (unsigned int)st.total_size,
                       (unsigned int)st.cache_free, (unsigned int)st.executed_lines,
                       (unsigned int)st.completed_lines, (unsigned int)st.total_lines);
        }
        if (g_host_job_topology_active) {
            host_sendf("@PROGRESS state=%u status=%u job=%u rx=%u total=%u free=%u lines=%u completed=%u total_lines=%u\r\n",
                       st.state, st.status, (unsigned int)st.job_id,
                       (unsigned int)st.received_size, (unsigned int)st.total_size,
                       (unsigned int)st.cache_free, (unsigned int)st.executed_lines,
                       (unsigned int)st.completed_lines, (unsigned int)st.total_lines);
        }
        if (g_host_job_topology_active && topology_state_is_terminal(st.state)) {
            clear_local_job_state();
            set_host_job_topology_active(false);
        }
        return;
    }

    if (pkt.type == PKT_PANEL_STATUS && pkt.len == sizeof(panel_status_payload_t)) {
        panel_status_payload_t st;
        memcpy(&st, pkt.payload, sizeof(st));
        if (JOB_DIAG_LOG) {
            osal_printk("[PANEL_STATUS] seq=%u owner=%u mode=%u state=%u flags=0x%02x job=%u rx=%u/%u lines=%u free=%u err=%u tick=%u\r\n",
                        (unsigned int)st.seq, st.owner, st.mode, st.job_state, st.flags,
                        (unsigned int)st.job_id, (unsigned int)st.received_size,
                        (unsigned int)st.total_size, (unsigned int)st.executed_lines,
                        (unsigned int)st.cache_free, (unsigned int)st.last_error,
                        (unsigned int)st.tick_ms);
        }
        if (g_host_job_topology_active && topology_state_is_terminal(st.job_state)) {
            clear_local_job_state();
            g_panel_local_terminal_confirmed = true;
            set_host_job_topology_active(false);
            tx_panel_publish_local_status(true);
        }
    }
}

static errcode_t send_job_begin(uint32_t job_id, uint32_t total_size, uint16_t crc,
                                 bool rx_auto_start, uint32_t exec_preroll_bytes,
                                 uint32_t total_lines)
{
    job_begin_stream_v2_payload_t begin = {0};
    begin.job_id = job_id;
    begin.total_size = total_size;
    begin.job_crc16 = crc;
    begin.options = rx_auto_start ? JOB_BEGIN_OPTION_AUTO_EXEC_PREROLL : 0U;
    begin.exec_preroll_bytes = rx_auto_start ? exec_preroll_bytes : 0U;
    begin.total_lines = total_lines;
    return send_packet_wait_ack(PKT_JOB_BEGIN, &begin, sizeof(begin));
}

static errcode_t send_job_end(void)
{
    job_end_payload_t end = {0};
    end.job_id = g_job_id;
    end.total_size = g_job_total;
    end.job_crc16 = g_job_crc;
    return send_packet_wait_ack_internal(PKT_JOB_END, &end, sizeof(end), false);
}

static bool status_confirms_job_end(const status_resp_payload_t *st)
{
    return status_matches_active_data_job(st) &&
           st->received_size >= g_job_total;
}

static bool stream_final_data_submitted(uint32_t target_offset)
{
#if JOB_TX_DATA_FAST_CUM_ACK_ENABLE
    unused(target_offset);
    return false;
#else
    return JOB_TX_DATA_ASYNC_AFTER_PREROLL &&
           g_preroll_signaled &&
           target_offset != 0U &&
           g_job_total != 0U &&
           target_offset == g_job_total &&
           g_job_offset >= g_job_total &&
           !g_async_data_error &&
           g_async_data_status == JOB_STATUS_OK;
#endif
}

static errcode_t send_job_end_robust(void)
{
#if !JOB_TX_DATA_FAST_CUM_ACK_ENABLE
    if (JOB_TX_DATA_ASYNC_AFTER_PREROLL && g_preroll_signaled &&
        g_async_data_ack_offset >= g_job_total) {
        osal_printk("[TX_JOB_END_SKIPPED_STREAM] t=%u job=%u rx_ack=%u total=%u "
                    "ack_seq=%u reason=final_data_ack_committed\r\n",
                    (unsigned int)uapi_systick_get_ms(),
                    (unsigned int)g_job_id,
                    (unsigned int)g_async_data_ack_offset,
                    (unsigned int)g_job_total,
                    (unsigned int)g_async_data_ack_seq);
        return ERRCODE_SUCC;
    }
#endif
    if (stream_final_data_submitted(g_job_total)) {
        osal_printk("[TX_JOB_END_SKIPPED_STREAM] t=%u job=%u rx_ack=%u total=%u "
                    "ack_seq=%u reason=final_data_submitted\r\n",
                    (unsigned int)uapi_systick_get_ms(),
                    (unsigned int)g_job_id,
                    (unsigned int)g_async_data_ack_offset,
                    (unsigned int)g_job_total,
                    (unsigned int)g_async_data_ack_seq);
        return ERRCODE_SUCC;
    }

    for (uint32_t attempt = 0; attempt <= JOB_TX_RETRY_MAX; attempt++) {
        errcode_t ret = send_job_end();
        if (ret == ERRCODE_SUCC) {
            return ERRCODE_SUCC;
        }

#if JOB_TX_DATA_WINDOW_STATUS_PROBE_ENABLE
        status_resp_payload_t st = {0};
        bool got_status = request_rx_status_sync("job_end", 0, g_job_total, &st,
                                                 JOB_TX_DATA_STATUS_PROBE_WAIT_MS);
        if (got_status && status_confirms_job_end(&st)) {
            osal_printk("[TX_JOB_END_STATUS_OK] t=%u attempt=%u job=%u rx=%u total=%u "
                        "state=%u status=%u\r\n",
                        (unsigned int)uapi_systick_get_ms(),
                        (unsigned int)attempt,
                        (unsigned int)st.job_id,
                        (unsigned int)st.received_size,
                        (unsigned int)st.total_size,
                        (unsigned int)st.state,
                        (unsigned int)st.status);
            return ERRCODE_SUCC;
        }
        osal_printk("[TX_JOB_END_RETRY] t=%u attempt=%u ret=0x%x got_status=%u "
                    "rx=%u total=%u state=%u status=%u\r\n",
                    (unsigned int)uapi_systick_get_ms(),
                    (unsigned int)attempt, (unsigned int)ret,
                    (unsigned int)(got_status ? 1U : 0U),
                    (unsigned int)(got_status ? st.received_size : 0U),
                    (unsigned int)(got_status ? st.total_size : 0U),
                    (unsigned int)(got_status ? st.state : 0U),
                    (unsigned int)(got_status ? st.status : 0U));
#endif
    }

    return ERRCODE_FAIL;
}

static bool wait_async_data_drain(uint32_t target_offset)
{
    if (target_offset == 0U) {
        return true;
    }
    if (!JOB_TX_DATA_ASYNC_AFTER_PREROLL) {
        return true;
    }
    if (!tx_data_async_stream_enabled()) {
        return true;
    }

    bool final_drain = target_offset == g_job_total &&
                       g_job_total > 0U && g_job_offset >= g_job_total;
    uint32_t now = (uint32_t)uapi_systick_get_ms();
    uint32_t start = (final_drain && g_final_data_submit_ms != 0U) ?
                     g_final_data_submit_ms : now;
#if !JOB_TX_DATA_FAST_CUM_ACK_ENABLE
    if (stream_final_data_submitted(target_offset)) {
        while (!g_async_data_error && g_async_data_ack_offset < target_offset) {
            uint32_t elapsed = (uint32_t)uapi_systick_get_ms() - start;
            if (elapsed >= JOB_TX_STREAM_FINAL_DRAIN_GRACE_MS) {
                osal_printk("[TX_DATA_ASYNC_DRAIN_DEFERRED_STREAM] target=%u ack_off=%u "
                            "ack_seq=%u status=%u waited=%u grace=%u reason=final_data_submitted\r\n",
                            (unsigned int)target_offset,
                            (unsigned int)g_async_data_ack_offset,
                            (unsigned int)g_async_data_ack_seq,
                            (unsigned int)g_async_data_status,
                            (unsigned int)elapsed,
                            (unsigned int)JOB_TX_STREAM_FINAL_DRAIN_GRACE_MS);
                return true;
            }
            osal_msleep(JOB_TX_DATA_WINDOW_POLL_MS);
        }

        if (g_async_data_error) {
            uint32_t waited = (uint32_t)uapi_systick_get_ms() - start;
            osal_printk("[TX_DATA_ASYNC_DRAIN_ERR] target=%u ack_off=%u ack_seq=%u status=%u waited=%u\r\n",
                        (unsigned int)target_offset,
                        (unsigned int)g_async_data_ack_offset,
                        (unsigned int)g_async_data_ack_seq,
                        (unsigned int)g_async_data_status,
                        (unsigned int)waited);
            return false;
        }
        return true;
    }
#endif

    uint32_t last_probe_elapsed_ms = 0;
    while (!g_async_data_error && g_async_data_ack_offset < target_offset) {
        uint32_t elapsed = (uint32_t)uapi_systick_get_ms() - start;
#if JOB_TX_DATA_WINDOW_STATUS_PROBE_ENABLE
        uint32_t first_probe_ms = final_drain ? JOB_TX_FINAL_DATA_STATUS_PROBE_MS :
                                               JOB_TX_DATA_WINDOW_STALL_MS;
        bool first_probe_due = last_probe_elapsed_ms == 0U && elapsed >= first_probe_ms;
        bool repeat_probe_due = last_probe_elapsed_ms != 0U &&
                                (uint32_t)(elapsed - last_probe_elapsed_ms) >=
                                JOB_TX_DATA_WINDOW_STALL_MS;
        if (first_probe_due || repeat_probe_due) {
            last_probe_elapsed_ms = elapsed;
            status_resp_payload_t st = {0};
            bool got_status = request_rx_status_sync(final_drain ? "final_data" : "data_drain",
                                                     0, target_offset, &st,
                                                     JOB_TX_DATA_STATUS_PROBE_WAIT_MS);
            if (final_drain && got_status && status_confirms_job_end(&st)) {
                osal_printk("[TX_FINAL_DATA_STATUS_OK] target=%u rx=%u state=%u waited=%u\r\n",
                            (unsigned int)target_offset,
                            (unsigned int)st.received_size,
                            (unsigned int)st.state,
                            (unsigned int)((uint32_t)uapi_systick_get_ms() - start));
            }
        }
#endif
        if (elapsed >= JOB_TX_ASYNC_DATA_DRAIN_TIMEOUT_MS) {
            osal_printk("[TX_DATA_ASYNC_DRAIN_TO] target=%u ack_off=%u ack_seq=%u status=%u waited=%u\r\n",
                        (unsigned int)target_offset,
                        (unsigned int)g_async_data_ack_offset,
                        (unsigned int)g_async_data_ack_seq,
                        (unsigned int)g_async_data_status,
                        (unsigned int)elapsed);
            return false;
        }
        osal_msleep(JOB_TX_DATA_WINDOW_POLL_MS);
    }

    uint32_t waited = (uint32_t)uapi_systick_get_ms() - start;
    if (g_async_data_error) {
        osal_printk("[TX_DATA_ASYNC_DRAIN_ERR] target=%u ack_off=%u ack_seq=%u status=%u waited=%u\r\n",
                    (unsigned int)target_offset,
                    (unsigned int)g_async_data_ack_offset,
                    (unsigned int)g_async_data_ack_seq,
                    (unsigned int)g_async_data_status,
                    (unsigned int)waited);
        return false;
    }
    if (waited >= JOB_TX_TIMING_SLOW_MS) {
        osal_printk("[TX_DATA_ASYNC_DRAIN_OK] target=%u ack_off=%u ack_seq=%u waited=%u\r\n",
                    (unsigned int)target_offset,
                    (unsigned int)g_async_data_ack_offset,
                    (unsigned int)g_async_data_ack_seq,
                    (unsigned int)waited);
    }
    return true;
}

static errcode_t send_job_data_chunk_async(const void *payload, uint16_t payload_len)
{
    uint8_t packet[SLE_JOB_PACKET_MAX_SIZE];
    uint16_t packet_len = 0;
    uint16_t seq = next_seq();
    uint16_t actual_payload_len = payload_len_for_type(PKT_JOB_DATA, payload, payload_len);
    const job_data_payload_t *dp = (const job_data_payload_t *)payload;
    uint32_t dbg_off = dp->offset;
    uint16_t dbg_dlen = dp->data_len;
    uint32_t next_offset = dbg_off + dbg_dlen;
    g_diag_data_count++;
    uint32_t dbg_data_index = g_diag_data_count;

    if (g_async_data_error) {
        osal_printk("[TX_DATA_ASYNC_BLOCKED] seq=%u off=%u len=%u ack_seq=%u ack_off=%u status=%u\r\n",
                    seq, (unsigned int)dbg_off, (unsigned int)dbg_dlen,
                    (unsigned int)g_async_data_ack_seq,
                    (unsigned int)g_async_data_ack_offset,
                    (unsigned int)g_async_data_status);
        return ERRCODE_FAIL;
    }

    uint8_t flags = JOB_TX_DATA_FAST_CUM_ACK_ENABLE ? SLE_JOB_PACKET_FLAG_DATA_FAST_ACK : 0U;
    if (JOB_TX_DATA_FAST_CUM_ACK_ENABLE && g_rx_auto_start_enabled &&
        (dbg_data_index % JOB_TX_DATA_WINDOW_EXEC_PACKETS) == 0U) {
        /* The RX default cumulative ACK interval is three packets. Force an
         * ACK at the two-packet execution window boundary to avoid deadlock. */
        flags |= SLE_JOB_PACKET_FLAG_DATA_FORCE_ACK;
    }
    if (JOB_TX_DATA_FAST_CUM_ACK_ENABLE && !g_rx_auto_start_enabled &&
        g_preroll_bytes > 0U &&
        !g_preroll_signaled && next_offset >= g_preroll_bytes) {
        flags |= SLE_JOB_PACKET_FLAG_DATA_FORCE_ACK;
    }
    if (!sle_packet_encode(PKT_JOB_DATA, flags, seq, payload, actual_payload_len,
                           packet, sizeof(packet), &packet_len)) {
        osal_printk("[JOB_TX] async encode fail type=0x%02x len=%u\r\n",
                    PKT_JOB_DATA, actual_payload_len);
        return ERRCODE_FAIL;
    }

    for (uint32_t retry = 0; retry <= JOB_TX_RETRY_MAX; retry++) {
        uint32_t reconnect_start = (uint32_t)uapi_systick_get_ms();
        while (!sle_job_client_is_connected()) {
            uint32_t reconnect_elapsed =
                (uint32_t)uapi_systick_get_ms() - reconnect_start;
            if (reconnect_elapsed >= JOB_TX_CONNECT_WAIT_MS) {
                osal_printk("[JOB_TX_WAIT_CONN_TIMEOUT] type=0x%02x seq=%u waited=%u status=%s async=1\r\n",
                            PKT_JOB_DATA, seq, (unsigned int)reconnect_elapsed,
                            sle_job_client_get_status());
                host_sendf("@NACK type=%u seq=%u status=%u reason=no_link async=1\r\n",
                           PKT_JOB_DATA, seq, JOB_STATUS_NOT_READY);
                return ERRCODE_FAIL;
            }
            sle_job_client_poll_connect();
            osal_msleep(20);
        }

        if (!wait_async_data_window(seq, dbg_data_index, dbg_off, dbg_dlen)) {
            break;
        }

        uint32_t t_send = (uint32_t)uapi_systick_get_ms();
        bool final_packet = g_job_total > 0U && next_offset >= g_job_total;
        if (final_packet) {
            g_final_data_submit_ms = t_send;
            g_final_data_ack_logged = false;
        }
        errcode_t ret = sle_job_client_send_packet_ex_timeout(
            packet, packet_len, final_packet, final_packet ?
            JOB_TX_FINAL_DATA_STATUS_PROBE_MS : JOB_SLE_WRITE_CFM_TIMEOUT_MS);
        uint32_t send_ms = (uint32_t)uapi_systick_get_ms() - t_send;
        bool final_submit_uncertain = final_packet && ret == ERRCODE_SLE_TIMEOUT;
        if ((ret == ERRCODE_SLE_SUCCESS || final_submit_uncertain) && !g_async_data_error) {
            uint32_t next_offset = dbg_off + dbg_dlen;
            uint32_t ack_offset = g_async_data_ack_offset;
            uint32_t outstanding = (next_offset > ack_offset) ? (next_offset - ack_offset) : 0U;
            if (!final_packet) {
                register_async_data_retx(packet, packet_len, seq, dbg_data_index,
                                         dbg_off, dbg_dlen,
                                         (uint32_t)uapi_systick_get_ms());
            } else {
                clear_async_data_retx();
            }
            if (final_packet) {
                osal_printk("[TX_FINAL_DATA_WRITE_REQ] seq=%u off=%u len=%u ret=0x%x "
                            "cfm_ms=%u uncertain=%u\r\n",
                            (unsigned int)seq, (unsigned int)dbg_off,
                            (unsigned int)dbg_dlen, (unsigned int)ret,
                            (unsigned int)send_ms,
                            (unsigned int)(final_submit_uncertain ? 1U : 0U));
            }
            if (tx_should_log_timing(PKT_JOB_DATA, dbg_data_index, send_ms)) {
                osal_printk("[TX_DATA_WIN] t=%u seq=%u data_idx=%u off=%u len=%u "
                            "next=%u send_ms=%u retry=%u ack_off=%u ack_seq=%u "
                            "outstanding=%u window=%u credit=%u link=%u client=%s\r\n",
                            (unsigned int)uapi_systick_get_ms(), seq,
                            (unsigned int)dbg_data_index,
                            (unsigned int)dbg_off, (unsigned int)dbg_dlen,
                            (unsigned int)next_offset,
                            (unsigned int)send_ms, (unsigned int)retry,
                            (unsigned int)ack_offset,
                            (unsigned int)g_async_data_ack_seq,
                            (unsigned int)outstanding,
                            (unsigned int)tx_async_data_window_bytes(),
                            (unsigned int)g_async_data_credit,
                            (unsigned int)sle_job_client_is_connected(),
                            sle_job_client_get_status());
            }
            return ERRCODE_SUCC;
        }
        if (final_packet) {
            g_final_data_submit_ms = 0;
        }

        osal_printk("[TX_DATA_ASYNC_SEND_FAIL] t=%u seq=%u data_idx=%u off=%u len=%u "
                    "retry=%u ret=0x%x send_ms=%u rx_err=%u ack_off=%u ack_seq=%u link=%u client=%s\r\n",
                    (unsigned int)uapi_systick_get_ms(), seq,
                    (unsigned int)dbg_data_index,
                    (unsigned int)dbg_off, (unsigned int)dbg_dlen,
                    (unsigned int)retry, (unsigned int)ret,
                    (unsigned int)send_ms,
                    (unsigned int)g_async_data_error,
                    (unsigned int)g_async_data_ack_offset,
                    (unsigned int)g_async_data_ack_seq,
                    (unsigned int)sle_job_client_is_connected(),
                    sle_job_client_get_status());
        if (g_async_data_error) {
            break;
        }
        osal_msleep(JOB_TX_DATA_WINDOW_POLL_MS);
    }

    host_sendf("@NACK type=%u seq=%u status=%u offset=%u async=1\r\n",
               PKT_JOB_DATA, seq, g_async_data_status, (unsigned int)dbg_off);
    return ERRCODE_FAIL;
}

static errcode_t send_job_data_chunk(uint32_t offset, const uint8_t *data, uint16_t chunk_len)
{
    if (chunk_len == 0 || data == NULL) {
        return ERRCODE_SUCC;
    }

    uint8_t payload[TX_PAYLOAD_BUF_SIZE];
    job_data_payload_t *p = (job_data_payload_t *)payload;
    p->job_id = g_job_id;
    p->offset = offset;
    p->data_len = chunk_len;
    memcpy(p->data, data, chunk_len);

    if (tx_data_async_stream_enabled()) {
        errcode_t ret = send_job_data_chunk_async(payload,
                                                  (uint16_t)(sizeof(job_data_payload_t) + chunk_len));
        if (JOB_DIAG_LOG && ret == ERRCODE_SUCC) {
            osal_printk("[JOB_TX_DATA_SENT] job=%u off=%u len=%u next=%u/%u async=1\r\n",
                        (unsigned int)g_job_id, (unsigned int)offset,
                        (unsigned int)chunk_len, (unsigned int)(offset + chunk_len),
                        (unsigned int)g_job_total);
        }
        return ret;
    }

    errcode_t ret = send_packet_wait_ack(PKT_JOB_DATA, payload,
                                         (uint16_t)(sizeof(job_data_payload_t) + chunk_len));
    if (JOB_DIAG_LOG && ret == ERRCODE_SUCC) {
        osal_printk("[JOB_TX_DATA_SENT] job=%u off=%u len=%u next=%u/%u\r\n",
                    (unsigned int)g_job_id, (unsigned int)offset,
                    (unsigned int)chunk_len, (unsigned int)(offset + chunk_len),
                    (unsigned int)g_job_total);
    }
    return ret;
}

static errcode_t flush_job_chunk(void)
{
    if (g_job_chunk_len == 0) {
        return ERRCODE_SUCC;
    }
    uint32_t offset = g_job_offset - g_job_chunk_len;
    uint16_t len = g_job_chunk_len;
    errcode_t ret = send_job_data_chunk(offset, g_job_chunk, g_job_chunk_len);
    if (ret == ERRCODE_SUCC) {
        g_job_chunk_len = 0;
    }
    if (JOB_DIAG_LOG && (g_diag_data_count <= JOB_DIAG_LOG_MAX_DATA || ret != ERRCODE_SUCC)) {
        osal_printk("[TX_CHUNK_DONE] t=%u off=%u len=%u ret=0x%x\r\n",
                    (unsigned int)uapi_systick_get_ms(), (unsigned int)offset,
                    (unsigned int)len, (unsigned int)ret);
    }
    return ret;
}

static void handle_data_byte(uint8_t ch)
{
    if (!g_data_mode) {
        return;
    }
    if (g_job_offset >= g_job_total) {
        return;
    }

    if (ch == '@' && g_preroll_bytes > 0 && !g_preroll_signaled) {
        osal_printk("[JOB_TX_DATA_SUSPICIOUS_AT] off=%u/%u chunk_len=%u preroll_signaled=%d\r\n",
                    (unsigned int)g_job_offset, (unsigned int)g_job_total,
                    (unsigned int)g_job_chunk_len, (int)g_preroll_signaled);
    }

    g_job_chunk[g_job_chunk_len++] = ch;
    g_job_offset++;

    if (g_job_offset >= g_data_log_next || g_job_offset >= g_job_total) {
        if (JOB_DIAG_LOG) {
            osal_printk("[JOB_TX_DATA_RX] job=%u off=%u/%u\r\n",
                        (unsigned int)g_job_id, (unsigned int)g_job_offset,
                        (unsigned int)g_job_total);
        }
        while (g_data_log_next <= g_job_offset) {
            g_data_log_next += TX_DATA_RX_LOG_STEP;
        }
    }

    if (!g_rx_auto_start_enabled && g_preroll_bytes > 0 && !g_preroll_signaled &&
        g_job_offset >= g_preroll_bytes && g_job_offset < g_job_total) {
        if (flush_job_chunk() != ERRCODE_SUCC) {
            osal_printk("[JOB_TX] preroll chunk send fail off=%u\r\n", (unsigned int)g_job_offset);
            (void)abort_rx_and_clear_transaction("preroll-chunk-send-fail");
            host_sendf("@ERR chunk_send_fail_abort\r\n");
            return;
        }
        if (!wait_async_data_drain(g_job_offset)) {
            osal_printk("[JOB_TX] preroll drain fail off=%u ack_off=%u\r\n",
                        (unsigned int)g_job_offset,
                        (unsigned int)g_async_data_ack_offset);
            (void)abort_rx_and_clear_transaction("preroll-drain-fail");
            host_sendf("@ERR preroll_drain_failed_abort\r\n");
            return;
        }
        g_preroll_signaled = true;
        g_async_data_ack_offset = g_job_offset;
        g_async_data_status = JOB_STATUS_OK;
        g_async_data_credit = 0;
        g_async_data_credit_valid = false;
        g_async_data_error = false;
        g_async_data_last_ack_ms = (uint32_t)uapi_systick_get_ms();
        clear_async_data_retx();
        g_panel_local_job_state = JOB_STATE_RECEIVING_JOB;
        g_data_mode = false;
        host_sendf("@ACK type=%u seq=0 status=%u offset=%u preroll=1\r\n",
                   PKT_JOB_DATA, JOB_STATUS_OK, (unsigned int)g_job_offset);
        tx_panel_publish_local_status(true);
        return;
    }

    if (g_job_chunk_len >= JOB_TX_DATA_CHUNK_MAX || g_job_offset >= g_job_total) {
        if (flush_job_chunk() != ERRCODE_SUCC) {
            osal_printk("[JOB_TX] chunk send fail off=%u\r\n", (unsigned int)g_job_offset);
            (void)abort_rx_and_clear_transaction("chunk-send-fail");
            host_sendf("@ERR chunk_send_fail_abort\r\n");
            return;
        }
        if (g_rx_auto_start_enabled && !g_panel_local_exec_started &&
            g_preroll_bytes > 0U && g_job_offset >= g_preroll_bytes) {
            g_panel_local_exec_started = true;
            g_panel_local_job_state = JOB_STATE_EXECUTING;
            osal_printk("[JOB_TX_AUTO_EXEC] threshold forwarded job=%u off=%u threshold=%u\r\n",
                        (unsigned int)g_job_id, (unsigned int)g_job_offset,
                        (unsigned int)g_preroll_bytes);
            tx_panel_queue_after_rx_window();
        }
        host_sendf("@ACK type=%u seq=0 status=%u offset=%u\r\n",
                   PKT_JOB_DATA, JOB_STATUS_OK, (unsigned int)g_job_offset);
        sle_job_client_poll_link_diagnostics();
        tx_poll_rx_status_for_panel();
        tx_panel_publish_local_status(false);
    }

    if (g_job_offset >= g_job_total) {
        g_data_mode = false;
        if (!wait_async_data_drain(g_job_total)) {
            (void)abort_rx_and_clear_transaction("job-data-drain-fail");
            host_sendf("@ERR job_data_drain_failed_abort\r\n");
            return;
        }
        if (send_job_end_robust() == ERRCODE_SUCC) {
            g_panel_local_job_state = g_panel_local_exec_started ?
                                      JOB_STATE_EXECUTING : JOB_STATE_JOB_READY;
            osal_printk("[JOB_TX] job upload complete job=%u size=%u crc=0x%04x\r\n",
                        (unsigned int)g_job_id, (unsigned int)g_job_total, g_job_crc);
            host_sendf("@JOB_READY job=%u size=%u\r\n", (unsigned int)g_job_id, (unsigned int)g_job_total);
            tx_panel_publish_local_status(true);
        } else {
            g_panel_local_last_error = JOB_STATUS_INTERNAL_ERROR;
            g_panel_local_job_state = JOB_STATE_ERROR;
            tx_panel_publish_local_status(true);
            (void)abort_rx_and_clear_transaction("job-end-fail");
            host_sendf("@ERR job_end_failed_abort\r\n");
        }
    }
}

static errcode_t send_simple_control(uint8_t type)
{
    errcode_t ret = send_packet_wait_ack(type, NULL, 0);
    if (ret == ERRCODE_SUCC) {
        if (type == PKT_EXEC_STOP) {
            g_panel_local_job_state = JOB_STATE_PAUSED;
        } else if (type == PKT_EXEC_RESUME) {
            g_panel_local_job_state = JOB_STATE_EXECUTING;
        }
        tx_panel_publish_local_status(true);
        host_sendf("@ACK type=%u seq=0 status=0\r\n", (unsigned int)type);
    }
    return ret;
}

static void send_focus_off_control(void)
{
    focus_ctrl_payload_t fp = {0};
    fp.on = 0;
    fp.power = 0;
    if (send_packet_wait_ack(PKT_FOCUS_CTRL, &fp, sizeof(fp)) == ERRCODE_SUCC) {
        host_sendf("@OK focus_off\r\n");
    } else {
        host_sendf("@NACK focus_reject\r\n");
    }
}

static void handle_uart_control_frame(uint8_t code)
{
    switch (code) {
        case JOB_TX_UART_CONTROL_EXEC_STOP:
            (void)send_simple_control(PKT_EXEC_STOP);
            return;
        case JOB_TX_UART_CONTROL_EXEC_RESUME:
            (void)send_simple_control(PKT_EXEC_RESUME);
            return;
        case JOB_TX_UART_CONTROL_ABORT:
            if (abort_rx_and_clear_transaction("uart-control-abort") == ERRCODE_SUCC) {
                host_sendf("@ACK type=%u seq=0 status=0\r\n", (unsigned int)PKT_JOB_ABORT);
            } else {
                host_sendf("@ERR abort_failed rx=unconfirmed\r\n");
            }
            return;
        case JOB_TX_UART_CONTROL_FOCUS_OFF:
            send_focus_off_control();
            return;
        default:
            osal_printk("[JOB_TX_CTRL_FRAME] unsupported code=0x%02x\r\n", code);
            host_sendf("@ERR bad_control_frame\r\n");
            return;
    }
}

static void send_route_switch_to_wifi(void)
{
    route_switch_payload_t payload = {0};
    payload.target_route = SLE_JOB_ROUTE_TARGET_LEGACY_WIFI;
    payload.flags = 0;
    payload.reserved = 0;

    host_sendf("@OK route_switch_request target=LEGACY_WIFI\r\n");
    osal_printk("[JOB_TX_ROUTE_SWITCH] target=LEGACY_WIFI\r\n");
    if (send_packet_wait_ack(PKT_ROUTE_SWITCH, &payload, sizeof(payload)) == ERRCODE_SUCC) {
        host_sendf("@OK route_switch_accepted target=LEGACY_WIFI\r\n");
    } else {
        host_sendf("@ERR route_switch_failed status=%u\r\n", (unsigned int)g_wait_status);
    }
}

static void handle_command_line(char *line)
{
    if (JOB_DIAG_LOG) {
        osal_printk("[JOB_TX_CMD] line=\"%s\" data_mode=%d off=%u/%u preroll_bytes=%u preroll_signaled=%d\r\n",
                    line, (int)g_data_mode, (unsigned int)g_job_offset,
                    (unsigned int)g_job_total, (unsigned int)g_preroll_bytes,
                    (int)g_preroll_signaled);
    }

    if (strncmp(line, "@BEGIN ", 7) == 0) {
        unsigned long job_id = 0;
        unsigned long total = 0;
        unsigned long crc = 0;
        unsigned long preroll = 0;
        unsigned long auto_start = 0;
        unsigned long total_lines = 0;
        const char *tag = " preroll=";
        const char *preroll_ptr = strstr(line, tag);
        const char *auto_tag = " auto_start=";
        const char *auto_ptr = strstr(line, auto_tag);
        const char *lines_tag = " lines=";
        const char *lines_ptr = strstr(line, lines_tag);
        int parsed = sscanf(line + 7, "%lu %lu %lx", &job_id, &total, &crc);
        if (parsed == 3 && preroll_ptr != NULL) {
            preroll = strtoul(preroll_ptr + strlen(tag), NULL, 10);
        }
        if (parsed == 3 && auto_ptr != NULL) {
            auto_start = strtoul(auto_ptr + strlen(auto_tag), NULL, 10);
        }
        if (parsed == 3 && lines_ptr != NULL) {
            total_lines = strtoul(lines_ptr + strlen(lines_tag), NULL, 10);
        }
        bool rx_auto_start = auto_start == 1U;
        if (JOB_DIAG_LOG) {
            osal_printk("[JOB_TX_BEGIN_PARSE] raw=\"%s\" job=%u total=%u crc=0x%04x "
                        "preroll=%u found_preroll=%d auto_start=%u\r\n",
                        line, (unsigned int)job_id, (unsigned int)total, (unsigned int)crc,
                        (unsigned int)preroll, (int)(preroll_ptr != NULL),
                        rx_auto_start ? 1U : 0U);
        }
        if (parsed != 3 || total == 0 || total_lines == 0 ||
            (auto_ptr != NULL && !rx_auto_start) ||
            (rx_auto_start && (preroll == 0 || preroll >= total))) {
            host_sendf("@ERR bad_begin\r\n");
            return;
        }
        set_host_job_topology_active(true);
        if (send_job_begin((uint32_t)job_id, (uint32_t)total, (uint16_t)crc,
                            rx_auto_start, (uint32_t)preroll,
                            (uint32_t)total_lines) != ERRCODE_SUCC) {
            (void)abort_rx_and_clear_transaction("job-begin-fail");
            host_sendf("@ERR begin_failed\r\n");
            return;
        }
        g_job_id = (uint32_t)job_id;
        g_job_total = (uint32_t)total;
        g_job_total_lines = (uint32_t)total_lines;
        g_job_crc = (uint16_t)crc;
        g_job_offset = 0;
        g_job_chunk_len = 0;
        g_data_log_next = TX_DATA_RX_LOG_STEP;
        g_data_mode = true;
        g_diag_data_count = 0;
        g_async_data_ack_offset = 0;
        g_async_data_ack_seq = 0;
        g_async_data_status = JOB_STATUS_OK;
        g_async_data_credit = 0;
        g_async_data_credit_valid = false;
        g_async_data_error = false;
        g_async_data_last_ack_ms = 0;
        clear_async_data_retx();
        g_preroll_bytes = (uint32_t)preroll;
        g_preroll_signaled = false;
        g_rx_auto_start_enabled = rx_auto_start;
        g_panel_local_job_state = JOB_STATE_RECEIVING_JOB;
        g_panel_local_exec_started = false;
        g_panel_local_last_error = JOB_STATUS_OK;
        g_panel_local_terminal_confirmed = false;
        memset(&g_panel_rx_status, 0, sizeof(g_panel_rx_status));
        g_panel_rx_status_valid = false;
        if (JOB_DIAG_LOG) {
            osal_printk("[JOB_TX_DATA_MODE] begin job=%u size=%u crc=0x%04x preroll=%u "
                        "auto_start=%u\r\n",
                        (unsigned int)g_job_id, (unsigned int)g_job_total, g_job_crc,
                        (unsigned int)g_preroll_bytes,
                        g_rx_auto_start_enabled ? 1U : 0U);
        }
        host_sendf("@DATA_READY job=%u size=%u auto_start=%s\r\n",
                   (unsigned int)g_job_id, (unsigned int)g_job_total,
                   g_rx_auto_start_enabled ? "rx" : "legacy");
        tx_panel_publish_local_status(true);
        return;
    }

    if (strncmp(line, "@EXEC_START ", 12) == 0) {
        exec_start_payload_t start = {0};
        start.job_id = (uint32_t)strtoul(line + 12, NULL, 0);
        if (send_packet_wait_ack(PKT_EXEC_START, &start, sizeof(start)) == ERRCODE_SUCC) {
            g_panel_local_exec_started = true;
            g_panel_local_job_state = JOB_STATE_EXECUTING;
            tx_panel_publish_local_status(true);
            host_sendf("@ACK type=16 seq=0 status=0\r\n");
        } else {
            g_panel_local_last_error = g_wait_status;
            tx_panel_publish_local_status(true);
            osal_printk("[JOB_TX_EXEC_START_FAIL] job=%u status=%u\r\n",
                        (unsigned int)start.job_id,
                        (unsigned int)g_wait_status);
            host_sendf("@ERR exec_start_failed status=%u\r\n", (unsigned int)g_wait_status);
        }
        return;
    }
    if (strcmp(line, "@DATA_RESUME") == 0) {
        if (g_preroll_signaled && g_job_offset < g_job_total && !g_data_mode) {
            if (!JOB_TX_DATA_ASYNC_AFTER_PREROLL) {
                g_async_data_ack_offset = g_job_offset;
            }
            g_async_data_status = JOB_STATUS_OK;
            g_async_data_credit = 0;
            g_async_data_credit_valid = false;
            g_async_data_error = false;
            g_async_data_last_ack_ms = (uint32_t)uapi_systick_get_ms();
            g_panel_local_exec_started = true;
            g_panel_local_job_state = JOB_STATE_EXECUTING;
#if JOB_TX_DATA_RESUME_GRACE_MS > 0
            {
                uint32_t t_grace = (uint32_t)uapi_systick_get_ms();
                osal_printk("[JOB_TX_DATA_RESUME_GRACE] start t=%u wait=%u off=%u/%u "
                            "ack_off=%u ack_seq=%u\r\n",
                            (unsigned int)t_grace,
                            (unsigned int)JOB_TX_DATA_RESUME_GRACE_MS,
                            (unsigned int)g_job_offset,
                            (unsigned int)g_job_total,
                            (unsigned int)g_async_data_ack_offset,
                            (unsigned int)g_async_data_ack_seq);
                osal_msleep(JOB_TX_DATA_RESUME_GRACE_MS);
                uint32_t t_done = (uint32_t)uapi_systick_get_ms();
                osal_printk("[JOB_TX_DATA_RESUME_GRACE] end t=%u waited=%u off=%u/%u "
                            "ack_off=%u ack_seq=%u\r\n",
                            (unsigned int)t_done,
                            (unsigned int)(t_done - t_grace),
                            (unsigned int)g_job_offset,
                            (unsigned int)g_job_total,
                            (unsigned int)g_async_data_ack_offset,
                            (unsigned int)g_async_data_ack_seq);
            }
#endif
            g_data_mode = true;
            host_sendf("@OK data_resume\r\n");
            tx_panel_publish_local_status(true);
        } else {
            osal_printk("[JOB_TX_DATA_RESUME] fail preroll_signaled=%d off=%u/%u data_mode=%d\r\n",
                        (int)g_preroll_signaled, (unsigned int)g_job_offset,
                        (unsigned int)g_job_total, (int)g_data_mode);
            host_sendf("@ERR cannot_resume\r\n");
        }
        return;
    }
    if (strcmp(line, "@EXEC_STOP") == 0) {
        send_simple_control(PKT_EXEC_STOP);
        return;
    }
    if (strcmp(line, "@EXEC_RESUME") == 0) {
        send_simple_control(PKT_EXEC_RESUME);
        return;
    }
    if (strcmp(line, "@ABORT") == 0) {
        if (abort_rx_and_clear_transaction("host-abort") == ERRCODE_SUCC) {
            host_sendf("@ACK type=%u seq=0 status=0\r\n", (unsigned int)PKT_JOB_ABORT);
        } else {
            host_sendf("@ERR abort_failed rx=unconfirmed\r\n");
        }
        return;
    }
    if (strcmp(line, "@RESET") == 0) {
        if (abort_rx_and_clear_transaction("host-reset") == ERRCODE_SUCC) {
            host_sendf("@OK reset rx=aborted\r\n");
        } else {
            host_sendf("@ERR reset_failed rx=unconfirmed\r\n");
        }
        return;
    }
    if (strcmp(line, "@STATUS") == 0) {
        if (g_rx_status_req_pending) {
            g_rx_status_report_host = true;
            return;
        }
        uint8_t packet[SLE_JOB_PACKET_MAX_SIZE];
        uint16_t packet_len = 0;
        uint16_t seq = peek_seq();
        if (sle_packet_encode(PKT_STATUS_REQ, 0, seq, NULL, 0, packet, sizeof(packet), &packet_len)) {
            if (sle_job_client_send_packet(packet, packet_len) == ERRCODE_SLE_SUCCESS) {
                g_rx_status_req_pending = true;
                g_rx_status_report_host = true;
                g_rx_status_req_ms = (uint32_t)uapi_systick_get_ms();
            }
        }
        return;
    }
    if (strcmp(line, "@RX MODE=GRBL") == 0) {
        if (g_data_mode) {
            host_sendf("@ERR route_switch_busy data_mode=1\r\n");
            return;
        }
        send_route_switch_to_wifi();
        return;
    }

    if (strncmp(line, "@FOCUS_ON S", 11) == 0) {
        unsigned long s = strtoul(line + 11, NULL, 10);
        if (s > 100) {
            host_sendf("@ERR focus_bad_power\r\n");
            return;
        }
        focus_ctrl_payload_t fp = {0};
        fp.on = 1;
        fp.power = (uint8_t)s;
        osal_printk("[FOCUS_TX] on s=%u sizeof=%u\r\n", (unsigned int)s, (unsigned int)sizeof(fp));
        if (send_packet_wait_ack(PKT_FOCUS_CTRL, &fp, sizeof(fp)) == ERRCODE_SUCC) {
            host_sendf("@OK focus_on s=%u\r\n", (unsigned int)s);
        } else {
            host_sendf("@NACK focus_reject\r\n");
        }
        return;
    }
    if (strcmp(line, "@FOCUS_OFF") == 0) {
        send_focus_off_control();
        return;
    }

    host_sendf("@ERR unknown_command\r\n");
}

static errcode_t job_uart_init(void)
{
#if defined(CONFIG_PINCTRL_SUPPORT_IE)
    uapi_pin_set_ie(LASER_UART_RX_PIN, PIN_IE_1);
#endif
    uapi_pin_set_mode(LASER_UART_TX_PIN, LASER_UART_PIN_MODE);
    uapi_pin_set_mode(LASER_UART_RX_PIN, LASER_UART_PIN_MODE);

    uart_attr_t attr = {0};
    attr.baud_rate = UART_BAUD_RATE;
    attr.data_bits = UART_DATA_BIT_8;
    attr.stop_bits = UART_STOP_BIT_1;
    attr.parity = UART_PARITY_NONE;

    uart_pin_config_t pin_cfg = {0};
    pin_cfg.tx_pin = LASER_UART_TX_PIN;
    pin_cfg.rx_pin = LASER_UART_RX_PIN;
    pin_cfg.cts_pin = PIN_NONE;
    pin_cfg.rts_pin = PIN_NONE;

    uapi_uart_deinit(LASER_UART_BUS);
    errcode_t ret = uapi_uart_init(LASER_UART_BUS, &pin_cfg, &attr, NULL, &g_uart_cfg);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[JOB_TX] uart init failed: 0x%x\r\n", ret);
        return ret;
    }

    ret = uapi_uart_register_rx_callback(
        LASER_UART_BUS, UART_RX_CONDITION_FULL_OR_SUFFICIENT_DATA_OR_IDLE,
        JOB_TX_UART_RX_BUF_SIZE, uart_rx_callback);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[JOB_TX] uart rx callback register failed: 0x%x\r\n", ret);
    } else {
        osal_printk("[JOB_TX_UART] rx=irq driver_buf=%u queue=%u\r\n",
                    (unsigned int)JOB_TX_UART_RX_BUF_SIZE,
                    (unsigned int)JOB_TX_UART_QUEUE_SIZE);
    }
    return ret;
}

static int uart_rx_task(void *arg)
{
    unused(arg);
    uint8_t ch;
    uint32_t data_idle_ticks = 0;
    uint32_t data_idle_start_ms = 0;
    uint32_t data_idle_last_log_ms = 0;
    uint32_t data_idle_last_busy_ms = 0;
    while (1) {
        if (g_uart_queue_error) {
            uint32_t lock = osal_irq_lock();
            if (!g_uart_queue_error) {
                osal_irq_restore(lock);
                continue;
            }
            uint32_t drops = g_uart_queue_drops;
            g_uart_queue_tail = g_uart_queue_head;
            osal_irq_restore(lock);

            if (g_data_mode || g_host_job_topology_active) {
                errcode_t reset_ret = abort_rx_and_clear_transaction("uart-rx-error");
                host_sendf("@ERR uart_rx_error drops=%u recovery=%s\r\n",
                           (unsigned int)drops,
                           (reset_ret == ERRCODE_SUCC) ? "safe" : "unconfirmed");
            } else {
                osal_printk("[JOB_TX_UART_RX_ERROR] drops=%u idle=1\r\n",
                            (unsigned int)drops);
            }

            lock = osal_irq_lock();
            g_uart_queue_error = false;
            g_uart_queue_discard = false;
            g_uart_queue_drops = 0;
            g_uart_queue_tail = g_uart_queue_head;
            osal_irq_restore(lock);
            data_idle_ticks = 0;
            data_idle_start_ms = 0;
            data_idle_last_log_ms = 0;
            data_idle_last_busy_ms = 0;
            continue;
        }

        if (!uart_queue_pop(&ch)) {
            if (g_data_mode) {
                uint32_t now = (uint32_t)uapi_systick_get_ms();
                if (data_idle_start_ms == 0U) {
                    data_idle_start_ms = now;
                }
                uint32_t idle_ms = (uint32_t)(now - data_idle_start_ms);
                if (g_job_offset < g_job_total) {
                    uint32_t chunk_base = (g_job_offset >= g_job_chunk_len) ?
                                          (g_job_offset - g_job_chunk_len) : g_job_offset;
                    uint32_t next_host_offset = min_u32(chunk_base + JOB_TX_DATA_CHUNK_MAX,
                                                        g_job_total);
                    uint32_t ack_offset = g_async_data_ack_offset;
                    uint32_t outstanding = (g_job_offset > ack_offset) ?
                                           (g_job_offset - ack_offset) : 0U;
                    if (g_panel_local_job_state != JOB_STATE_PAUSED &&
                        idle_ms >= JOB_TX_DATA_UART_IDLE_LOG_MS &&
                        (data_idle_last_log_ms == 0U ||
                         (uint32_t)(now - data_idle_last_log_ms) >= JOB_TX_DATA_UART_IDLE_LOG_MS)) {
                        data_idle_last_log_ms = now;
                        osal_printk("[TX_DATA_RX_IDLE] t=%u idle_ms=%u off=%u/%u "
                                    "chunk_base=%u chunk_len=%u next_ack=%u ack_off=%u "
                                    "outstanding=%u data_mode=%u preroll=%u\r\n",
                                    (unsigned int)now,
                                    (unsigned int)idle_ms,
                                    (unsigned int)g_job_offset,
                                    (unsigned int)g_job_total,
                                    (unsigned int)chunk_base,
                                    (unsigned int)g_job_chunk_len,
                                    (unsigned int)next_host_offset,
                                    (unsigned int)ack_offset,
                                    (unsigned int)outstanding,
                                    (unsigned int)(g_data_mode ? 1U : 0U),
                                    (unsigned int)(g_preroll_signaled ? 1U : 0U));
                    }
                    if (g_panel_local_job_state != JOB_STATE_PAUSED &&
                        idle_ms >= JOB_TX_DATA_UART_IDLE_BUSY_MS &&
                        (data_idle_last_busy_ms == 0U ||
                         (uint32_t)(now - data_idle_last_busy_ms) >= JOB_TX_DATA_UART_IDLE_BUSY_MS)) {
                        data_idle_last_busy_ms = now;
                        host_sendf("@BUSY type=%u offset=%u ack_offset=%u outstanding=%u "
                                   "window=%u waited=%u partial=%u uart_idle=1\r\n",
                                   PKT_JOB_DATA,
                                   (unsigned int)next_host_offset,
                                   (unsigned int)ack_offset,
                                   (unsigned int)outstanding,
                                    (unsigned int)tx_async_data_window_bytes(),
                                    (unsigned int)idle_ms,
                                    (unsigned int)g_job_chunk_len);
                    }
                    if (g_job_chunk_len > 0U && idle_ms >= JOB_TX_DATA_UART_IDLE_BUSY_MS) {
                        uint32_t partial_offset = g_job_offset;
                        if (flush_job_chunk() == ERRCODE_SUCC) {
                            host_sendf("@ACK type=%u seq=0 status=%u offset=%u partial=1\r\n",
                                       PKT_JOB_DATA, JOB_STATUS_OK,
                                       (unsigned int)partial_offset);
                            data_idle_ticks = 0;
                            data_idle_start_ms = 0;
                            data_idle_last_log_ms = 0;
                            data_idle_last_busy_ms = 0;
                            continue;
                        }
                        osal_printk("[JOB_TX] idle partial flush fail off=%u len=%u\r\n",
                                    (unsigned int)partial_offset,
                                    (unsigned int)g_job_chunk_len);
                        (void)abort_rx_and_clear_transaction("idle-partial-flush-fail");
                        host_sendf("@ERR partial_flush_failed_abort\r\n");
                        data_idle_ticks = 0;
                        data_idle_start_ms = 0;
                        data_idle_last_log_ms = 0;
                        data_idle_last_busy_ms = 0;
                        continue;
                    }
                }
                data_idle_ticks++;
                if (data_idle_ticks >= TX_DATA_MODE_TIMEOUT_TICKS) {
                    errcode_t reset_ret = abort_rx_and_clear_transaction("data-mode-timeout");
                    data_idle_ticks = 0;
                    data_idle_start_ms = 0;
                    data_idle_last_log_ms = 0;
                    data_idle_last_busy_ms = 0;
                    host_sendf("@ERR data_mode_timeout recovery=%s\r\n",
                               (reset_ret == ERRCODE_SUCC) ? "safe" : "unconfirmed");
                }
            } else {
                data_idle_start_ms = 0;
                data_idle_last_log_ms = 0;
                data_idle_last_busy_ms = 0;
            }
            sle_job_client_poll_connect();
            tx_poll_rx_status_for_panel();
            tx_panel_publish_local_status(false);
            if (g_uart_queue_wait_ready) {
                (void)osal_wait_timeout_uninterruptible(
                    &g_uart_queue_wait, uart_queue_ready, NULL,
                    JOB_TX_UART_READ_TIMEOUT_MS);
            } else {
                osal_msleep(1);
            }
            continue;
        }
        data_idle_ticks = 0;
        data_idle_start_ms = 0;
        data_idle_last_log_ms = 0;
        data_idle_last_busy_ms = 0;

        /* ASCII CAN is an out-of-band transaction reset in every parser mode. */
        if (ch == JOB_TX_UART_RESYNC_BYTE) {
            handle_uart_resync();
            continue;
        }

        if (g_uart_control_frame_active) {
            g_uart_control_frame_active = false;
            handle_uart_control_frame(ch);
            continue;
        }

        if (ch == JOB_TX_UART_CONTROL_BYTE) {
            g_uart_control_frame_active = true;
            continue;
        }

        if (g_data_mode) {
            handle_data_byte(ch);
            continue;
        }

        if (ch == '\n' || ch == '\r') {
            if (g_line_len > 0) {
                g_line[g_line_len] = '\0';
                handle_command_line(g_line);
                g_line_len = 0;
            }
        } else if (g_line_len < (TX_LINE_MAX - 1U)) {
            g_line[g_line_len++] = (char)ch;
        } else {
            osal_printk("[JOB_TX_CMD] line too long, reset parser\r\n");
            g_line_len = 0;
            host_sendf("@ERR line_too_long\r\n");
        }
    }
    return 0;
}

static void create_task(const char *name, osal_kthread_handler entry, uint32_t stack_size, uint32_t prio)
{
    osal_kthread_lock();
    osal_task *task = osal_kthread_create(entry, NULL, name, stack_size);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[JOB_TX] create task %s failed\r\n", name);
        return;
    }
    if (osal_kthread_set_priority(task, prio) != OSAL_SUCCESS) {
        osal_printk("[JOB_TX] set task priority %s failed\r\n", name);
    }
    osal_kfree(task);
    osal_kthread_unlock();
}

static int sle_init_task(void *arg)
{
    unused(arg);

    osal_msleep(500);

    sle_job_client_set_response_cb(response_cb);

    errcode_t ret = sle_job_client_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[JOB_TX_BOOT] sle client init failed: 0x%x\r\n", ret);
    } else {
        sle_job_client_set_panel_link_allowed(true);
    }
    return (ret == ERRCODE_SUCC) ? 0 : -1;
}

static void laser_sle_job_tx_entry(void)
{
    osal_printk("[FW_ID] board=TX firmware=%s app=ws63_laser_sle_job/transmitter role=host-uart-to-sle payload=%u mtu=%u uart=%u sle_interval_units=0x%02x panel_interval_units=0x%02x\r\n",
                TX_FIRMWARE_PACKAGE,
                (unsigned int)JOB_TX_DATA_CHUNK_MAX,
                (unsigned int)JOB_SLE_MTU_SIZE,
                (unsigned int)UART_BAUD_RATE,
                (unsigned int)JOB_SLE_CONN_INTERVAL_UNITS,
                (unsigned int)JOB_SLE_PANEL_CONN_INTERVAL_UNITS);
    if (osal_sem_init(&g_ack_sem, 0) == OSAL_SUCCESS) {
        g_ack_sem_ready = true;
    } else {
        osal_printk("[JOB_TX_BOOT] ack sem init failed\r\n");
    }
    if (osal_sem_init(&g_status_sem, 0) == OSAL_SUCCESS) {
        g_status_sem_ready = true;
    } else {
        osal_printk("[JOB_TX_BOOT] status sem init failed\r\n");
    }
    if (osal_wait_init(&g_uart_queue_wait) == OSAL_SUCCESS) {
        g_uart_queue_wait_ready = true;
    } else {
        osal_printk("[JOB_TX_BOOT] uart queue wait init failed\r\n");
        return;
    }
    if (job_uart_init() != ERRCODE_SUCC) {
        return;
    }
    host_sendf("@FW board=TX firmware=%s app=ws63_laser_sle_job/transmitter role=host-uart-to-sle payload=%u mtu=%u uart=%u sle_interval_units=0x%02x panel_interval_units=0x%02x\r\n",
               TX_FIRMWARE_PACKAGE,
               (unsigned int)JOB_TX_DATA_CHUNK_MAX,
               (unsigned int)JOB_SLE_MTU_SIZE,
               (unsigned int)UART_BAUD_RATE,
               (unsigned int)JOB_SLE_CONN_INTERVAL_UNITS,
               (unsigned int)JOB_SLE_PANEL_CONN_INTERVAL_UNITS);
    create_task("job_uart_rx", uart_rx_task, TASK_STACK_SIZE_DEFAULT, TASK_PRIO_JOB_UART);
    create_task("job_sle_init", sle_init_task, TASK_STACK_SIZE_SLE, TASK_PRIO_SLE);
}

app_run(laser_sle_job_tx_entry);
