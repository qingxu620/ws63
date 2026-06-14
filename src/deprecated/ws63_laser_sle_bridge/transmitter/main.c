/**
 * @file main.c
 * @brief Transmitter for the SLE transparent serial bridge.
 *
 * This board deliberately does not implement Grbl. It only forwards bytes:
 *   PC/LaserGRBL UART RX -> SLE write
 *   SLE notify           -> PC/LaserGRBL UART TX
 */
#include "app_init.h"
#include "common_def.h"
#include "config.h"
#include "errcode.h"
#include "pinctrl.h"
#include "protocol.h"
#include "sle_errcode.h"
#include "sle_passthrough.h"
#include "soc_osal.h"
#include "systick.h"
#include "uart.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define TX_DIAG_LINE_MAX 96

static uint8_t g_uart_rx_buf[SLE_BRIDGE_UART_RX_BUF_SIZE];
static uart_buffer_config_t g_uart_buf_cfg = {
    .rx_buffer = g_uart_rx_buf,
    .rx_buffer_size = SLE_BRIDGE_UART_RX_BUF_SIZE,
};
static volatile bool g_host_seen_since_link = false;
static bool g_last_sle_ready = false;
static char g_tx_line_buf[TX_DIAG_LINE_MAX];
static uint16_t g_tx_line_pos = 0;
static char g_tx_last_line[TX_DIAG_LINE_MAX];
static volatile uint32_t g_tx_last_line_ms = 0;
static volatile unsigned long g_tx_line_seq = 0;
static volatile unsigned long g_tx_sle_send_retry = 0;
static volatile unsigned long g_tx_sle_send_fail = 0;
static volatile unsigned long g_tx_sle_send_drop = 0;
static volatile unsigned long g_tx_uart_queue_drop = 0;
static volatile uint16_t g_tx_next_frame_seq = 1;
static volatile uint16_t g_tx_last_ack_seq = 0;
static volatile uint16_t g_tx_rx_credit = 0;
static volatile bool g_tx_rx_credit_valid = false;
static volatile unsigned long g_tx_frame_retx = 0;
static volatile unsigned long g_tx_frame_sent = 0;
static volatile unsigned long g_tx_ack_count = 0;
static volatile unsigned long g_tx_ack_timeout = 0;
static volatile unsigned long g_tx_status_drop = 0;
static volatile unsigned long g_tx_resp_uart_written = 0;
static volatile unsigned long g_tx_ack_ignored = 0;
static volatile unsigned long g_tx_credit_limited = 0;
static volatile uint16_t g_tx_wait_ack_seq = 0;
static volatile bool g_tx_wait_ack_active = false;
static volatile bool g_tx_wait_ack_seen = false;
static uint8_t g_tx_uart_queue[SLE_BRIDGE_TX_QUEUE_SIZE];
static uint16_t g_tx_uart_q_head = 0;
static uint16_t g_tx_uart_q_tail = 0;
static bool g_tx_pending_status_query = false;
static bool g_tx_uart_q_ready = false;
static osal_mutex g_tx_uart_q_mutex;
static osal_semaphore g_tx_uart_q_sem;
static osal_semaphore g_tx_ack_sem;
static bool g_tx_ack_sem_ready = false;

static uint16_t tx_uart_q_used(void);

#if SLE_BRIDGE_DEBUG_TRACE
static bool tx_trace_should_log(unsigned long count)
{
    return count <= 16UL || (count % SLE_BRIDGE_TRACE_FRAME_PERIOD) == 0UL;
}
#endif

static uint16_t uart_write_raw(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        return 0;
    }

    uint16_t offset = 0;
    uint8_t no_progress = 0;
    while (offset < len) {
        uint16_t remain = (uint16_t)(len - offset);
        uint16_t chunk = (remain > SLE_BRIDGE_UART_WRITE_CHUNK_MAX) ?
            SLE_BRIDGE_UART_WRITE_CHUNK_MAX : remain;
        int32_t written = uapi_uart_write(LASER_UART_BUS, &data[offset], chunk,
                                          SLE_BRIDGE_UART_WRITE_TIMEOUT_MS);
        if (written <= 0) {
            no_progress++;
            if (no_progress >= 3U) {
#if SLE_BRIDGE_DEBUG_TRACE
                osal_printk("[TX_UART_WRITE_SHORT] want=%u done=%u ret=%d\r\n",
                            (unsigned int)len, (unsigned int)offset, (int)written);
#endif
                break;
            }
            osal_msleep(1);
            continue;
        }
        no_progress = 0;
        offset = (uint16_t)(offset + (uint16_t)written);
        if ((uint16_t)written < chunk) {
#if SLE_BRIDGE_DEBUG_TRACE
            osal_printk("[TX_UART_WRITE_PART] chunk=%u wrote=%d done=%u/%u\r\n",
                        (unsigned int)chunk, (int)written, (unsigned int)offset,
                        (unsigned int)len);
#endif
            osal_msleep(1);
        }
    }

    return offset;
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static bool bridge_frame_is_ack(const uint8_t *data, uint16_t len)
{
    return data != NULL && len >= SLE_BRIDGE_FRAME_HEADER_LEN &&
           data[0] == SLE_BRIDGE_FRAME_MAGIC0 && data[1] == SLE_BRIDGE_FRAME_MAGIC1 &&
           data[2] == SLE_BRIDGE_FRAME_TYPE_ACK;
}

static bool tx_handle_ack_frame(const uint8_t *data, uint16_t len)
{
    if (!bridge_frame_is_ack(data, len)) {
        return false;
    }

    g_tx_last_ack_seq = get_le16(&data[3]);
    g_tx_rx_credit = get_le16(&data[5]);
    g_tx_rx_credit_valid = true;
    g_tx_ack_count++;
    bool matches_waiter = g_tx_wait_ack_active && g_tx_last_ack_seq == g_tx_wait_ack_seq;
    if (matches_waiter) {
        g_tx_wait_ack_seen = true;
    } else {
        g_tx_ack_ignored++;
    }
#if SLE_BRIDGE_DEBUG_TRACE
    if (tx_trace_should_log(g_tx_ack_count)) {
        osal_printk("[TX_ACK] seq=%u credit=%u ack=%u sent=%u retx=%u timeout=%u ignored=%u q=%u\r\n",
                    (unsigned int)g_tx_last_ack_seq, (unsigned int)g_tx_rx_credit,
                    (unsigned int)g_tx_ack_count, (unsigned int)g_tx_frame_sent,
                    (unsigned int)g_tx_frame_retx, (unsigned int)g_tx_ack_timeout,
                    (unsigned int)g_tx_ack_ignored, (unsigned int)tx_uart_q_used());
    }
#endif
    if (matches_waiter && g_tx_ack_sem_ready) {
        osal_sem_up(&g_tx_ack_sem);
    }
    return true;
}

static bool tx_ack_received(uint16_t seq)
{
    return g_tx_wait_ack_active && g_tx_wait_ack_seen && g_tx_last_ack_seq == seq;
}

static bool tx_wait_sle_connected(uint32_t timeout_ms)
{
    uint32_t start_ms = (uint32_t)uapi_systick_get_ms();
    while (!sle_passthrough_is_connected()) {
        if ((uint32_t)uapi_systick_get_ms() - start_ms >= timeout_ms) {
            return false;
        }
        osal_msleep(10);
    }
    return true;
}

static void tx_wait_ack_begin(uint16_t seq)
{
    while (g_tx_ack_sem_ready && osal_sem_down_timeout(&g_tx_ack_sem, 0) == OSAL_SUCCESS) {
    }
    g_tx_wait_ack_seq = seq;
    g_tx_wait_ack_seen = (g_tx_last_ack_seq == seq);
    g_tx_wait_ack_active = true;
}

static void tx_wait_ack_end(void)
{
    g_tx_wait_ack_active = false;
    g_tx_wait_ack_seen = false;
}

static bool tx_send_frame_wait_ack(const uint8_t *payload, uint16_t len)
{
    if (payload == NULL || len == 0) {
        return true;
    }

    uint8_t frame[SLE_BRIDGE_FRAME_HEADER_LEN + SLE_BRIDGE_FRAME_PAYLOAD_MAX];
    uint16_t seq = g_tx_next_frame_seq++;
    if (g_tx_next_frame_seq == 0) {
        g_tx_next_frame_seq = 1;
    }

    frame[0] = SLE_BRIDGE_FRAME_MAGIC0;
    frame[1] = SLE_BRIDGE_FRAME_MAGIC1;
    frame[2] = SLE_BRIDGE_FRAME_TYPE_DATA;
    put_le16(&frame[3], seq);
    put_le16(&frame[5], len);
    frame[7] = 0;
    memcpy(&frame[SLE_BRIDGE_FRAME_HEADER_LEN], payload, len);

    bool sent_ok = false;
    for (uint8_t i = 0; i < SLE_BRIDGE_SEND_RETRY_MAX; i++) {
        if (!tx_wait_sle_connected(SLE_BRIDGE_LINK_WAIT_MS)) {
            break;
        }
        tx_wait_ack_begin(seq);
        if (tx_ack_received(seq)) {
            sent_ok = true;
            break;
        }
        errcode_t ret = sle_passthrough_send_cmd(frame, (uint16_t)(SLE_BRIDGE_FRAME_HEADER_LEN + len));
        if (ret != ERRCODE_SUCC) {
            tx_wait_ack_end();
            g_tx_sle_send_retry++;
#if SLE_BRIDGE_DEBUG_TRACE
            osal_printk("[TX_SEND_ERR] seq=%u len=%u try=%u ret=0x%x retry=%u q=%u\r\n",
                        (unsigned int)seq, (unsigned int)len, (unsigned int)i,
                        ret, (unsigned int)g_tx_sle_send_retry, (unsigned int)tx_uart_q_used());
#endif
            osal_msleep(SLE_BRIDGE_SEND_RETRY_DELAY_MS);
            continue;
        }
        g_tx_frame_sent++;
#if SLE_BRIDGE_DEBUG_TRACE
        if (tx_trace_should_log(g_tx_frame_sent)) {
            osal_printk("[TX_FRAME] seq=%u len=%u try=%u credit=%u sent=%u q=%u\r\n",
                        (unsigned int)seq, (unsigned int)len, (unsigned int)i,
                        (unsigned int)g_tx_rx_credit, (unsigned int)g_tx_frame_sent,
                        (unsigned int)tx_uart_q_used());
        }
#endif
        if (tx_ack_received(seq)) {
            sent_ok = true;
            break;
        }
        if (g_tx_ack_sem_ready &&
            osal_sem_down_timeout(&g_tx_ack_sem, SLE_BRIDGE_FRAME_ACK_TIMEOUT_MS) == OSAL_SUCCESS &&
            tx_ack_received(seq)) {
            sent_ok = true;
            break;
        }

        tx_wait_ack_end();
        g_tx_sle_send_retry++;
        g_tx_frame_retx++;
        g_tx_ack_timeout++;
#if SLE_BRIDGE_DEBUG_TRACE
        osal_printk("[TX_RETX] seq=%u len=%u try=%u last_ack=%u retry=%u retx=%u timeout=%u q=%u\r\n",
                    (unsigned int)seq, (unsigned int)len, (unsigned int)i,
                    (unsigned int)g_tx_last_ack_seq, (unsigned int)g_tx_sle_send_retry,
                    (unsigned int)g_tx_frame_retx, (unsigned int)g_tx_ack_timeout,
                    (unsigned int)tx_uart_q_used());
#endif
    }

    if (sent_ok) {
        tx_wait_ack_end();
        return true;
    }

    g_tx_sle_send_fail++;
#if SLE_BRIDGE_DEBUG_TRACE
    osal_printk("[TX_FAIL] seq=%u len=%u last_ack=%u fail=%u drop=%u q=%u\r\n",
                (unsigned int)seq, (unsigned int)len, (unsigned int)g_tx_last_ack_seq,
                (unsigned int)g_tx_sle_send_fail, (unsigned int)g_tx_sle_send_drop,
                (unsigned int)tx_uart_q_used());
#endif
    return false;
}

static uint16_t tx_uart_q_next(uint16_t value)
{
    return (uint16_t)((value + 1U) % SLE_BRIDGE_TX_QUEUE_SIZE);
}

static bool tx_uart_q_push_byte(uint8_t ch)
{
    if (!g_tx_uart_q_ready) {
        return false;
    }

    bool ok = false;
    osal_mutex_lock(&g_tx_uart_q_mutex);
    uint16_t next = tx_uart_q_next(g_tx_uart_q_head);
    if (next != g_tx_uart_q_tail) {
        g_tx_uart_queue[g_tx_uart_q_head] = ch;
        g_tx_uart_q_head = next;
        ok = true;
    }
    osal_mutex_unlock(&g_tx_uart_q_mutex);
    return ok;
}

static uint16_t tx_uart_q_pop_bytes(uint8_t *out, uint16_t max_len)
{
    if (out == NULL || max_len == 0 || !g_tx_uart_q_ready) {
        return 0;
    }

    uint16_t count = 0;
    osal_mutex_lock(&g_tx_uart_q_mutex);
    while (g_tx_uart_q_tail != g_tx_uart_q_head && count < max_len) {
        out[count++] = g_tx_uart_queue[g_tx_uart_q_tail];
        g_tx_uart_q_tail = tx_uart_q_next(g_tx_uart_q_tail);
    }
    osal_mutex_unlock(&g_tx_uart_q_mutex);
    return count;
}

static uint16_t tx_uart_q_used(void)
{
    if (!g_tx_uart_q_ready) {
        return 0;
    }

    uint16_t used;
    osal_mutex_lock(&g_tx_uart_q_mutex);
    if (g_tx_uart_q_head >= g_tx_uart_q_tail) {
        used = (uint16_t)(g_tx_uart_q_head - g_tx_uart_q_tail);
    } else {
        used = (uint16_t)(SLE_BRIDGE_TX_QUEUE_SIZE - g_tx_uart_q_tail + g_tx_uart_q_head);
    }
    osal_mutex_unlock(&g_tx_uart_q_mutex);
    return used;
}

static void tx_uart_q_clear(void)
{
    if (!g_tx_uart_q_ready) {
        return;
    }

    osal_mutex_lock(&g_tx_uart_q_mutex);
    g_tx_uart_q_head = 0;
    g_tx_uart_q_tail = 0;
    g_tx_pending_status_query = false;
    while (osal_sem_down_timeout(&g_tx_uart_q_sem, 0) == OSAL_SUCCESS) {
    }
    osal_mutex_unlock(&g_tx_uart_q_mutex);
}

static void tx_defer_status_query(void)
{
    if (!g_tx_uart_q_ready) {
        return;
    }

    osal_mutex_lock(&g_tx_uart_q_mutex);
    g_tx_pending_status_query = true;
    osal_mutex_unlock(&g_tx_uart_q_mutex);
}

static bool tx_take_pending_status_query_if_idle(void)
{
    if (!g_tx_uart_q_ready) {
        return false;
    }

    bool take = false;
    osal_mutex_lock(&g_tx_uart_q_mutex);
    if (g_tx_pending_status_query && g_tx_uart_q_head == g_tx_uart_q_tail) {
        g_tx_pending_status_query = false;
        take = true;
    }
    osal_mutex_unlock(&g_tx_uart_q_mutex);
    return take;
}

static bool tx_send_raw_to_sle(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        return true;
    }

    uint16_t offset = 0;
    while (offset < len) {
        uint16_t remain = (uint16_t)(len - offset);
        uint16_t chunk = (remain > SLE_BRIDGE_FRAME_PAYLOAD_MAX) ? SLE_BRIDGE_FRAME_PAYLOAD_MAX : remain;
        if (g_tx_rx_credit_valid && g_tx_rx_credit > 0 && g_tx_rx_credit < chunk) {
            chunk = g_tx_rx_credit;
            g_tx_credit_limited++;
#if SLE_BRIDGE_DEBUG_TRACE
            if (tx_trace_should_log(g_tx_credit_limited)) {
                osal_printk("[TX_CREDIT_LIMIT] chunk=%u remain=%u credit=%u limited=%u\r\n",
                            (unsigned int)chunk, (unsigned int)remain,
                            (unsigned int)g_tx_rx_credit, (unsigned int)g_tx_credit_limited);
            }
#endif
        }
        if (!tx_send_frame_wait_ack(&data[offset], chunk)) {
            g_tx_sle_send_drop += (unsigned long)(len - offset);
            return false;
        }
        offset = (uint16_t)(offset + chunk);
        if (SLE_BRIDGE_TX_CHUNK_GAP_MS > 0 && offset < len) {
            osal_msleep(SLE_BRIDGE_TX_CHUNK_GAP_MS);
        }
    }

    return true;
}

static uint16_t tx_queue_host_bytes(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        return 0;
    }

    uint16_t accepted = 0;
    for (uint16_t i = 0; i < len; i++) {
        if (data[i] == 0x18) {
            tx_uart_q_clear();
        }
        if (data[i] == '?' && (tx_uart_q_used() > 0 || g_tx_wait_ack_active)) {
            g_tx_status_drop++;
            tx_defer_status_query();
#if SLE_BRIDGE_DEBUG_TRACE
            if (tx_trace_should_log(g_tx_status_drop)) {
                osal_printk("[TX_DEFER_RT] ch='?' defer=%u q=%u\r\n",
                            (unsigned int)g_tx_status_drop, (unsigned int)tx_uart_q_used());
            }
#endif
            accepted++;
            continue;
        }
        if (!tx_uart_q_push_byte(data[i])) {
            g_tx_uart_queue_drop += (unsigned long)(len - i);
            break;
        }
        osal_sem_up(&g_tx_uart_q_sem);
        accepted++;
    }
    return accepted;
}

static bool tx_diag_is_printable(uint8_t ch)
{
    return ch >= 0x20 && ch <= 0x7e;
}

static void tx_diag_finish_line(void)
{
    if (g_tx_line_pos == 0) {
        return;
    }

    g_tx_line_buf[g_tx_line_pos] = '\0';
    snprintf(g_tx_last_line, sizeof(g_tx_last_line), "%s", g_tx_line_buf);
    g_tx_last_line_ms = (uint32_t)uapi_systick_get_ms();
    g_tx_line_seq++;
#if SLE_BRIDGE_TIMING_VERBOSE
    osal_printk("[BRIDGE_TIMING_TX] id=%u line=%s t1_ms=%u\r\n",
                (unsigned int)g_tx_line_seq, g_tx_last_line, (unsigned int)g_tx_last_line_ms);
#endif
#if SLE_BRIDGE_DEBUG_TRACE
    osal_printk("[TX_PC_LINE] id=%u line=\"%s\" q=%u ack=%u sent=%u drop_rt=%u\r\n",
                (unsigned int)g_tx_line_seq, g_tx_last_line, (unsigned int)tx_uart_q_used(),
                (unsigned int)g_tx_ack_count, (unsigned int)g_tx_frame_sent,
                (unsigned int)g_tx_status_drop);
#endif
    g_tx_line_pos = 0;
}

static void tx_diag_record_pc_bytes(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        return;
    }

    for (uint16_t i = 0; i < len; i++) {
        uint8_t ch = data[i];
        if (ch == 0x18 || ch == '?' || ch == '!' || ch == '~') {
#if SLE_BRIDGE_DEBUG_TRACE
            if (ch == '?') {
                osal_printk("[TX_RT] ch='?' q=%u ack_wait=%u drop_rt=%u\r\n",
                            (unsigned int)tx_uart_q_used(),
                            (unsigned int)(g_tx_wait_ack_active ? 1U : 0U),
                            (unsigned int)g_tx_status_drop);
            } else if (ch == 0x18) {
                osal_printk("[TX_RT] ch=0x18 q=%u ack_wait=%u\r\n",
                            (unsigned int)tx_uart_q_used(),
                            (unsigned int)(g_tx_wait_ack_active ? 1U : 0U));
            } else {
                osal_printk("[TX_RT] ch='%c' q=%u ack_wait=%u\r\n", (char)ch,
                            (unsigned int)tx_uart_q_used(),
                            (unsigned int)(g_tx_wait_ack_active ? 1U : 0U));
            }
#endif
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            tx_diag_finish_line();
            continue;
        }

        if (g_tx_line_pos < TX_DIAG_LINE_MAX - 1) {
            g_tx_line_buf[g_tx_line_pos++] = tx_diag_is_printable(ch) ? (char)ch : '.';
        } else {
            g_tx_line_buf[TX_DIAG_LINE_MAX - 2] = '~';
        }
    }
}

static void on_sle_response(const uint8_t *data, uint16_t length)
{
    if (tx_handle_ack_frame(data, length)) {
        return;
    }

    uint16_t uart_written = uart_write_raw(data, length);
    g_tx_resp_uart_written++;

#if SLE_BRIDGE_TIMING_VERBOSE || SLE_BRIDGE_DEBUG_TRACE
    uint16_t log_len = (length < 96U) ? length : 96U;
    char log_buf[128];
    uint16_t pos = 0;
    for (uint16_t j = 0; j < log_len && pos < sizeof(log_buf) - 2; j++) {
        char c = (char)data[j];
        if (c == '\r') { log_buf[pos++] = '\\'; log_buf[pos++] = 'r'; }
        else if (c == '\n') { log_buf[pos++] = '\\'; log_buf[pos++] = 'n'; }
        else if (c >= 0x20 && c <= 0x7e) { log_buf[pos++] = c; }
    }
    log_buf[pos] = '\0';
#if SLE_BRIDGE_TIMING_VERBOSE
    osal_printk("[TX_PC_RESP] len=%u uart=%u/%u %s%s\r\n", (unsigned int)length,
                (unsigned int)uart_written, (unsigned int)length, log_buf,
                (length > log_len) ? "..." : "");
#else
    if (tx_trace_should_log(g_tx_resp_uart_written) || uart_written != length) {
        osal_printk("[TX_UART_RESP] n=%u len=%u uart=%u/%u %s%s\r\n",
                    (unsigned int)g_tx_resp_uart_written, (unsigned int)length,
                    (unsigned int)uart_written, (unsigned int)length, log_buf,
                    (length > log_len) ? "..." : "");
    }
#endif
#endif
}

static errcode_t bridge_uart_init(void)
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
    errcode_t ret = uapi_uart_init(LASER_UART_BUS, &pin_cfg, &attr, NULL, &g_uart_buf_cfg);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[bridge tx] uart init failed: 0x%x\r\n", ret);
        return ret;
    }

    osal_printk("[bridge tx] uart ready bus=%d tx=GPIO%d rx=GPIO%d baud=%d\r\n",
                LASER_UART_BUS, LASER_UART_TX_PIN, LASER_UART_RX_PIN, UART_BAUD_RATE);
    return ERRCODE_SUCC;
}

static errcode_t tx_uart_queue_init(void)
{
    g_tx_uart_q_head = 0;
    g_tx_uart_q_tail = 0;
    g_tx_uart_queue_drop = 0;
    if (osal_mutex_init(&g_tx_uart_q_mutex) != OSAL_SUCCESS ||
        osal_sem_init(&g_tx_uart_q_sem, 0) != OSAL_SUCCESS) {
        osal_printk("[bridge tx] uart queue init failed\r\n");
        return ERRCODE_FAIL;
    }
    if (osal_sem_init(&g_tx_ack_sem, 0) != OSAL_SUCCESS) {
        osal_printk("[bridge tx] ack sem init failed\r\n");
        return ERRCODE_FAIL;
    }
    g_tx_uart_q_ready = true;
    g_tx_ack_sem_ready = true;
    return ERRCODE_SUCC;
}

static int uart_rx_task(void *arg)
{
    unused(arg);
    uint8_t ch;

    while (1) {
        if (tx_uart_q_used() >= SLE_BRIDGE_TX_QUEUE_BACKPRESSURE_BYTES) {
            osal_msleep(SLE_BRIDGE_UART_RX_PAUSE_MS);
            continue;
        }

        int32_t len = uapi_uart_read(LASER_UART_BUS, &ch, 1, SLE_BRIDGE_UART_READ_TIMEOUT_MS);
        if (len <= 0) {
            osal_msleep(1);
            continue;
        }

#if SLE_BRIDGE_TIMING_VERBOSE
        osal_printk("[TX_UART_BYTE] 0x%02x '%c'\r\n", ch, tx_diag_is_printable(ch) ? (char)ch : '.');
#endif

        tx_diag_record_pc_bytes(&ch, 1);

        if (!g_host_seen_since_link) {
            g_host_seen_since_link = true;
        }

        uint16_t pushed = tx_queue_host_bytes(&ch, 1);
        if (pushed < 1U) {
            osal_printk("[bridge tx] uart queue overflow bytes_in=1 queued=%u drop=%u\r\n",
                        (unsigned int)pushed, (unsigned int)g_tx_uart_queue_drop);
        }
        osal_yield();
    }

    return 0;
}

static int uart_to_sle_task(void *arg)
{
    unused(arg);
    uint8_t buf[SLE_BRIDGE_UART_CHUNK_MAX];

    while (1) {
        if (!sle_passthrough_is_connected()) {
            osal_msleep(5);
            continue;
        }

        if (tx_uart_q_used() == 0 &&
            osal_sem_down_timeout(&g_tx_uart_q_sem, 10) != OSAL_SUCCESS) {
            osal_msleep(1);
            continue;
        }

        if (!sle_passthrough_is_connected()) {
            if (tx_uart_q_used() > 0) {
                osal_sem_up(&g_tx_uart_q_sem);
            }
            continue;
        }

        uint32_t send_start_ms = (uint32_t)uapi_systick_get_ms();
        unsigned long retry_before = g_tx_sle_send_retry;
        unsigned long fail_before = g_tx_sle_send_fail;
        uint16_t total = 0;
        uint16_t accepted = 0;
        uint16_t len = 0;
        do {
            len = tx_uart_q_pop_bytes(buf, sizeof(buf));
            if (len == 0) {
                break;
            }
            total = (uint16_t)(total + len);
            if (!tx_send_raw_to_sle(buf, len)) {
                break;
            }
            accepted = (uint16_t)(accepted + len);
        } while (len == sizeof(buf));

        if (accepted == total && tx_take_pending_status_query_if_idle()) {
            uint8_t status_query = '?';
            total++;
            if (tx_send_raw_to_sle(&status_query, 1)) {
                accepted++;
            }
        }
#if SLE_BRIDGE_TIMING_VERBOSE
        osal_printk("[BRIDGE_TIMING_TX_SLE] bytes_in=%d bytes_sent=%u sle_send_ms=%u retry_delta=%u fail_delta=%u\r\n",
                    (int)total, (unsigned int)accepted,
                    (unsigned int)((uint32_t)uapi_systick_get_ms() - send_start_ms),
                    (unsigned int)(g_tx_sle_send_retry - retry_before),
                    (unsigned int)(g_tx_sle_send_fail - fail_before));
#else
        if (accepted < total || g_tx_sle_send_fail != fail_before) {
            osal_printk("[bridge tx] sle send short bytes_in=%d bytes_sent=%u sle_send_ms=%u retry_delta=%u fail_delta=%u drop=%u retx=%u\r\n",
                        (int)total, (unsigned int)accepted,
                        (unsigned int)((uint32_t)uapi_systick_get_ms() - send_start_ms),
                        (unsigned int)(g_tx_sle_send_retry - retry_before),
                        (unsigned int)(g_tx_sle_send_fail - fail_before),
                        (unsigned int)g_tx_sle_send_drop, (unsigned int)g_tx_frame_retx);
        }
#endif
    }

    return 0;
}

static int sle_poll_task(void *arg)
{
    unused(arg);
    osal_msleep(500);
    (void)sle_passthrough_init();

    while (1) {
        sle_passthrough_poll_connect();
        bool sle_ready = sle_passthrough_is_connected();
        if (sle_ready && !g_last_sle_ready) {
            g_host_seen_since_link = false;
            g_tx_rx_credit = 0;
            g_tx_rx_credit_valid = false;
            if (!g_tx_wait_ack_active && tx_uart_q_used() == 0) {
                g_tx_next_frame_seq = 1;
                g_tx_last_ack_seq = 0;
            }
        } else if (!sle_ready && g_last_sle_ready) {
            g_host_seen_since_link = false;
            g_tx_rx_credit = 0;
            g_tx_rx_credit_valid = false;
            if (g_tx_ack_sem_ready) {
                osal_sem_up(&g_tx_ack_sem);
            }
        }
        g_last_sle_ready = sle_ready;
        osal_msleep(100);
    }
    return 0;
}

static void create_task(const char *name, int (*entry)(void *), unsigned short prio)
{
    osal_kthread_lock();
    osal_task *task = osal_kthread_create(entry, NULL, name, TASK_STACK_SIZE_DEFAULT);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[bridge tx] create %s failed\r\n", name);
        return;
    }
    if (osal_kthread_set_priority(task, prio) != OSAL_SUCCESS) {
        osal_printk("[bridge tx] set %s priority failed\r\n", name);
    }
    osal_kfree(task);
    osal_kthread_unlock();
}

static void laser_sle_bridge_tx_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 Laser SLE Bridge TX\r\n");
    osal_printk("  mode: transparent UART <-> SLE\r\n");
    osal_printk("========================================\r\n");

    if (bridge_uart_init() != ERRCODE_SUCC) {
        return;
    }
    if (tx_uart_queue_init() != ERRCODE_SUCC) {
        return;
    }

    sle_passthrough_set_response_cb(on_sle_response);

    create_task("bridge_uart_rx", uart_rx_task, TASK_PRIO_BRIDGE_UART_RX);
    create_task("bridge_sle_tx", uart_to_sle_task, TASK_PRIO_BRIDGE_SLE_TX);
    create_task("bridge_sle", sle_poll_task, TASK_PRIO_UART + 1);
}

app_run(laser_sle_bridge_tx_entry);
