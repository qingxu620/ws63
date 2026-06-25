/**
 * @file uart_transport.c
 * @brief USART Direct transport for unified RX.
 */
#include "uart_transport.h"
#include "common_def.h"
#include "config.h"
#include "pinctrl.h"
#include "rx_core.h"
#include "soc_osal.h"
#include "uart.h"
#include <string.h>

#define UART_RX_BUF_SIZE 4096
#define UART_READ_TIMEOUT_MS 20

static uint8_t g_uart_rx_buff[UART_RX_BUF_SIZE] = {0};
static uart_buffer_config_t g_uart_buffer_config = {
    .rx_buffer = g_uart_rx_buff,
    .rx_buffer_size = UART_RX_BUF_SIZE,
};

void uart_transport_write(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0) {
        return;
    }
    (void)uapi_uart_write(RX_UART_BUS, data, len, 0);
}

void uart_transport_write_str(const char *str)
{
    if (str == NULL) {
        return;
    }
    uart_transport_write((const uint8_t *)str, (uint32_t)strlen(str));
}

static int uart_transport_rx_task(void *arg)
{
    unused(arg);

    osal_msleep(500);
    rx_core_on_stream_ready(RX_SRC_UART);

    uint8_t ch;
    while (1) {
        int32_t ret = uapi_uart_read(RX_UART_BUS, &ch, 1, UART_READ_TIMEOUT_MS);
        if (ret <= 0) {
            rx_core_on_stream_poll(RX_SRC_UART);
            osal_msleep(1);
            continue;
        }

        rx_core_on_stream_byte(RX_SRC_UART, ch);
    }

    return 0;
}

errcode_t uart_transport_init(void)
{
#if defined(CONFIG_PINCTRL_SUPPORT_IE)
    uapi_pin_set_ie(RX_UART_RX_PIN, PIN_IE_1);
#endif
    uapi_pin_set_mode(RX_UART_TX_PIN, RX_UART_PIN_MODE);
    uapi_pin_set_mode(RX_UART_RX_PIN, RX_UART_PIN_MODE);

    uart_attr_t attr = {0};
    attr.baud_rate = RX_UART_BAUD_RATE;
    attr.data_bits = UART_DATA_BIT_8;
    attr.stop_bits = UART_STOP_BIT_1;
    attr.parity = UART_PARITY_NONE;

    uart_pin_config_t pin_cfg = {0};
    pin_cfg.tx_pin = RX_UART_TX_PIN;
    pin_cfg.rx_pin = RX_UART_RX_PIN;
    pin_cfg.cts_pin = PIN_NONE;
    pin_cfg.rts_pin = PIN_NONE;

    uapi_uart_deinit(RX_UART_BUS);
    errcode_t ret = uapi_uart_init(RX_UART_BUS, &pin_cfg, &attr, NULL, &g_uart_buffer_config);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[UART] init failed: 0x%x\r\n", ret);
        return ret;
    }

    return ERRCODE_SUCC;
}

errcode_t uart_transport_start_task(void)
{
    osal_kthread_lock();
    osal_task *task = osal_kthread_create(uart_transport_rx_task, NULL, "rx_uart", TASK_STACK_SIZE_DEFAULT);
    if (task == NULL) {
        osal_kthread_unlock();
        osal_printk("[UART] create rx task failed\r\n");
        return ERRCODE_FAIL;
    }

    if (osal_kthread_set_priority(task, TASK_PRIO_UART) != OSAL_SUCCESS) {
        osal_printk("[UART] set rx task priority failed\r\n");
    }

    osal_kfree(task);
    osal_kthread_unlock();
    return ERRCODE_SUCC;
}
