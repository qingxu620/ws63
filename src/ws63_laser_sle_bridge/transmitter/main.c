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
static volatile unsigned long g_tx_resp_uart_written = 0;
static volatile unsigned long g_tx_max_resp_delay_ms = 0;

static void uart_write_raw(const uint8_t *data, uint16_t len)
{
    if (data != NULL && len > 0) {
        (void)uapi_uart_write(LASER_UART_BUS, data, len, 0);
    }
}

static bool sle_send_with_retry(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        return true;
    }

    for (uint8_t i = 0; i < SLE_BRIDGE_SEND_RETRY_MAX && sle_passthrough_is_connected(); i++) {
        if (sle_passthrough_send_line((const char *)data, len) == ERRCODE_SUCC) {
            return true;
        }
        g_tx_sle_send_retry++;
        osal_msleep(SLE_BRIDGE_SEND_RETRY_DELAY_MS);
    }

    g_tx_sle_send_fail++;
    return false;
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
    osal_printk("[BRIDGE_TIMING_TX] id=%lu line=\"%s\" t1_ms=%lu\r\n",
                g_tx_line_seq, g_tx_last_line, (unsigned long)g_tx_last_line_ms);
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
    uint32_t start_ms = (uint32_t)uapi_systick_get_ms();
    uart_write_raw(data, length);
    uint32_t now_ms = (uint32_t)uapi_systick_get_ms();
    uint32_t since_line_ms = (g_tx_last_line_ms == 0) ? 0 : (uint32_t)(now_ms - g_tx_last_line_ms);

    g_tx_resp_uart_written++;
    if (since_line_ms > g_tx_max_resp_delay_ms) {
        g_tx_max_resp_delay_ms = since_line_ms;
    }
#if SLE_BRIDGE_TIMING_VERBOSE
    uint32_t uart_write_ms = (uint32_t)(now_ms - start_ms);
    osal_printk("[BRIDGE_TIMING_TX_RESP] last_line=\"%s\" resp_len=%u since_pc_line_ms=%lu uart_write_ms=%lu resp_uart_written=%lu tx_sle_send_retry=%lu tx_sle_send_fail=%lu max_resp_delay_ms=%lu\r\n",
                g_tx_last_line, (unsigned int)length, (unsigned long)since_line_ms,
                (unsigned long)uart_write_ms, g_tx_resp_uart_written, g_tx_sle_send_retry,
                g_tx_sle_send_fail, g_tx_max_resp_delay_ms);
#else
    unused(start_ms);
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

static int uart_to_sle_task(void *arg)
{
    unused(arg);
    uint8_t buf[SLE_BRIDGE_UART_CHUNK_MAX];

    while (1) {
        int32_t len = uapi_uart_read(LASER_UART_BUS, buf, sizeof(buf), SLE_BRIDGE_UART_READ_TIMEOUT_MS);
        if (len <= 0) {
            osal_msleep(1);
            continue;
        }

        if (!sle_passthrough_is_connected()) {
            continue;
        }

        tx_diag_record_pc_bytes(buf, (uint16_t)len);

        if (!g_host_seen_since_link) {
            g_host_seen_since_link = true;
        }

        uint16_t offset = 0;
        uint32_t send_start_ms = (uint32_t)uapi_systick_get_ms();
        unsigned long retry_before = g_tx_sle_send_retry;
        unsigned long fail_before = g_tx_sle_send_fail;
        while (offset < (uint16_t)len) {
            if (!sle_passthrough_is_connected()) {
                g_tx_sle_send_drop += (unsigned long)((uint16_t)len - offset);
                break;
            }
            uint16_t remain = (uint16_t)((uint16_t)len - offset);
            uint16_t chunk = (remain > SLE_BRIDGE_UART_CHUNK_MAX) ? SLE_BRIDGE_UART_CHUNK_MAX : remain;
            while (!sle_send_with_retry(&buf[offset], chunk)) {
                if (!sle_passthrough_is_connected()) {
                    g_tx_sle_send_drop += (unsigned long)((uint16_t)len - offset);
                    break;
                }
                osal_msleep(SLE_BRIDGE_SEND_RETRY_DELAY_MS);
            }
            if (!sle_passthrough_is_connected()) {
                break;
            }
            offset = (uint16_t)(offset + chunk);
            if (SLE_BRIDGE_TX_CHUNK_GAP_MS > 0) {
                osal_msleep(SLE_BRIDGE_TX_CHUNK_GAP_MS);
            }
        }
#if SLE_BRIDGE_TIMING_VERBOSE
        osal_printk("[BRIDGE_TIMING_TX_SLE] bytes_in=%d bytes_sent=%u sle_send_ms=%lu retry_delta=%lu fail_delta=%lu\r\n",
                    (int)len, (unsigned int)offset,
                    (unsigned long)((uint32_t)uapi_systick_get_ms() - send_start_ms),
                    g_tx_sle_send_retry - retry_before, g_tx_sle_send_fail - fail_before);
#else
        if (offset < (uint16_t)len || g_tx_sle_send_fail != fail_before) {
            osal_printk("[bridge tx] sle send short bytes_in=%d bytes_sent=%u sle_send_ms=%lu retry_delta=%lu fail_delta=%lu drop=%lu\r\n",
                        (int)len, (unsigned int)offset,
                        (unsigned long)((uint32_t)uapi_systick_get_ms() - send_start_ms),
                        g_tx_sle_send_retry - retry_before,
                        g_tx_sle_send_fail - fail_before, g_tx_sle_send_drop);
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
        } else if (!sle_ready && g_last_sle_ready) {
            g_host_seen_since_link = false;
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

    sle_passthrough_set_response_cb(on_sle_response);

    create_task("bridge_uart", uart_to_sle_task, TASK_PRIO_UART);
    create_task("bridge_sle", sle_poll_task, TASK_PRIO_UART + 1);
}

app_run(laser_sle_bridge_tx_entry);
