/**
 * @file main.c
 * @brief Transmitter entry - bidirectional SLE-UART bridge.
 *
 * This board is a "wireless serial port":
 *   UART RX → SLE TX (G-code to receiver)
 *   SLE RX → UART TX (ok/error/status from receiver)
 */
#include "app_init.h"
#include "common_def.h"
#include "errcode.h"
#include "pinctrl.h"
#include "soc_osal.h"
#include "systick.h"
#include "uart.h"
#include "sle_passthrough.h"
#include <string.h>

/* UART config */
#define TX_UART_BUS 1
#define TX_UART_TX_PIN 15
#define TX_UART_RX_PIN 16
#define TX_UART_PIN_MODE 1
#define TX_UART_BAUD 115200

#define RX_BUF_SIZE 4096
#define LINE_MAX 128
#define READ_TIMEOUT_MS 20

static uint8_t g_uart_rx_buf[RX_BUF_SIZE];
static uart_buffer_config_t g_uart_buf_cfg = {
    .rx_buffer = g_uart_rx_buf,
    .rx_buffer_size = RX_BUF_SIZE
};

static char g_line[LINE_MAX];
static int g_line_pos = 0;

/* Response callback: SLE RX → UART TX */
static void on_sle_response(const uint8_t *data, uint16_t length)
{
    if (data != NULL && length > 0) {
        /* Forward response back to PC via UART */
        uapi_uart_write(TX_UART_BUS, data, length, 0);
    }
}

static void uart_send_str(const char *str)
{
    uint32_t len = (uint32_t)strlen(str);
    if (len > 0) {
        uapi_uart_write(TX_UART_BUS, (const uint8_t *)str, len, 0);
    }
}

static void process_char(uint8_t ch)
{
    if (ch == '\n' || ch == '\r') {
        if (g_line_pos > 0) {
            g_line[g_line_pos] = '\0';

            /* Forward to SLE if connected */
            if (sle_passthrough_is_connected()) {
                errcode_t ret = sle_passthrough_send_line(g_line, (uint16_t)g_line_pos);
                if (ret != ERRCODE_SUCC) {
                    uart_send_str("error:sle_send\r\n");
                }
                /* Note: ok/error will come from receiver via SLE */
            } else {
                uart_send_str("error:not_connected\r\n");
            }
            g_line_pos = 0;
        }
    } else if (g_line_pos < LINE_MAX - 1) {
        g_line[g_line_pos++] = (char)ch;
    }
}

/* SLE init task */
static int sle_init_task(void *arg)
{
    unused(arg);
    osal_msleep(500);

    /* Set response callback before init */
    sle_passthrough_set_response_cb(on_sle_response);

    errcode_t ret = sle_passthrough_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[tx] SLE init failed: 0x%x\r\n", ret);
    }
    return 0;
}

/* UART RX task */
static int uart_rx_task(void *arg)
{
    unused(arg);
    osal_printk("[tx] uart rx task started\r\n");
    osal_msleep(100);

    /* Send startup banner */
    uart_send_str("\r\nWS63 SLE Transmitter (Bidirectional Bridge)\r\n");
    uart_send_str("Waiting for SLE connection...\r\n");

    uint8_t ch;
    while (1) {
        int32_t ret = uapi_uart_read(TX_UART_BUS, &ch, 1, READ_TIMEOUT_MS);
        if (ret <= 0) {
            osal_msleep(1);
            continue;
        }

        /* Echo status on '?' */
        if (ch == '?') {
            /* Forward to receiver via SLE */
            if (sle_passthrough_is_connected()) {
                sle_passthrough_send_line("?", 1);
            }
            continue;
        }

        /* Handle Ctrl-X (soft reset) */
        if (ch == 0x18) {
            if (sle_passthrough_is_connected()) {
                sle_passthrough_send_line("\x18", 1);
            }
            continue;
        }

        /* Handle realtime commands */
        if (ch == '!' || ch == '~') {
            if (sle_passthrough_is_connected()) {
                char cmd[2] = {(char)ch, '\0'};
                sle_passthrough_send_line(cmd, 1);
            }
            continue;
        }

        process_char(ch);
    }
    return 0;
}

static errcode_t uart_init(void)
{
#if defined(CONFIG_PINCTRL_SUPPORT_IE)
    uapi_pin_set_ie(TX_UART_RX_PIN, PIN_IE_1);
#endif
    uapi_pin_set_mode(TX_UART_TX_PIN, TX_UART_PIN_MODE);
    uapi_pin_set_mode(TX_UART_RX_PIN, TX_UART_PIN_MODE);

    uart_attr_t attr = {0};
    attr.baud_rate = TX_UART_BAUD;
    attr.data_bits = UART_DATA_BIT_8;
    attr.stop_bits = UART_STOP_BIT_1;
    attr.parity = UART_PARITY_NONE;

    uart_pin_config_t pin_cfg = {0};
    pin_cfg.tx_pin = TX_UART_TX_PIN;
    pin_cfg.rx_pin = TX_UART_RX_PIN;
    pin_cfg.cts_pin = PIN_NONE;
    pin_cfg.rts_pin = PIN_NONE;

    uapi_uart_deinit(TX_UART_BUS);
    errcode_t ret = uapi_uart_init(TX_UART_BUS, &pin_cfg, &attr, NULL, &g_uart_buf_cfg);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[tx] uart init failed: 0x%x\r\n", ret);
        return ret;
    }
    osal_printk("[tx] uart init OK bus=%d baud=%d\r\n", TX_UART_BUS, TX_UART_BAUD);
    return ERRCODE_SUCC;
}

static void sle_transmitter_entry(void)
{
    osal_printk("========================================\r\n");
    osal_printk("  WS63 SLE Transmitter (Bidirectional)\r\n");
    osal_printk("========================================\r\n");

    errcode_t ret = uart_init();
    if (ret != ERRCODE_SUCC) {
        return;
    }

    osal_kthread_lock();

    /* Create SLE init task */
    osal_task *task = osal_kthread_create(sle_init_task, NULL, "sle_init", 0x2000);
    if (task != NULL) {
        osal_kthread_set_priority(task, 26);
        osal_kfree(task);
    }

    /* Create UART RX task */
    task = osal_kthread_create(uart_rx_task, NULL, "uart_rx", 0x4000);
    if (task != NULL) {
        osal_kthread_set_priority(task, 3);
        osal_kfree(task);
    }

    osal_kthread_unlock();
    osal_printk("[tx] ready\r\n");
}

app_run(sle_transmitter_entry);
