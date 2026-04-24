/**
 * @file zdt_uart.c
 * @brief ZDT 控制板 UART/RS485 传输层
 */
#include "zdt_uart.h"
#include "config.h"
#include "gpio.h"
#include "pinctrl.h"
#include "soc_osal.h"
#include "systick.h"
#include <string.h>

static zdt_uart_config_t g_zdt_uart_config;
static uint8_t g_uart_rx_buffer[ZDT_UART_RX_BUFFER_SIZE] = {0};
static uart_buffer_config_t g_uart_buffer_config = {
    .rx_buffer = g_uart_rx_buffer,
    .rx_buffer_size = ZDT_UART_RX_BUFFER_SIZE,
};

static void zdt_uart_set_rs485_direction(bool transmit)
{
    gpio_level_t level;

    if (!g_zdt_uart_config.rs485_dir_enable) {
        return;
    }

    if (transmit) {
        level = g_zdt_uart_config.rs485_dir_active_high ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW;
    } else {
        level = g_zdt_uart_config.rs485_dir_active_high ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH;
    }
    uapi_gpio_set_val(g_zdt_uart_config.rs485_dir_pin, level);
}

static uint32_t zdt_uart_tx_settle_delay_us(uint16_t length)
{
    uint32_t bit_time_us;
    uint32_t frame_time_us;

    if (g_zdt_uart_config.baud_rate == 0U) {
        return 2000U;
    }

    bit_time_us = (1000000U + g_zdt_uart_config.baud_rate - 1U) / g_zdt_uart_config.baud_rate;
    frame_time_us = (uint32_t)length * 10U * bit_time_us;
    return frame_time_us + ZDT_RS485_TURNAROUND_DELAY_US;
}

errcode_t zdt_uart_init(const zdt_uart_config_t *config)
{
    uart_attr_t attr = {0};
    uart_pin_config_t pin_cfg = {0};
    errcode_t ret;

    if (config == NULL) {
        return ERRCODE_INVALID_PARAM;
    }

    g_zdt_uart_config = *config;

#if defined(CONFIG_PINCTRL_SUPPORT_IE)
    uapi_pin_set_ie(g_zdt_uart_config.rx_pin, PIN_IE_1);
#endif
    uapi_pin_set_mode(g_zdt_uart_config.tx_pin, g_zdt_uart_config.pin_mode);
    uapi_pin_set_mode(g_zdt_uart_config.rx_pin, g_zdt_uart_config.pin_mode);

    if (g_zdt_uart_config.rs485_dir_enable) {
        uapi_gpio_init();
        uapi_pin_set_mode(g_zdt_uart_config.rs485_dir_pin, HAL_PIO_FUNC_GPIO);
        uapi_gpio_set_dir(g_zdt_uart_config.rs485_dir_pin, GPIO_DIRECTION_OUTPUT);
        zdt_uart_set_rs485_direction(false);
    }

    attr.baud_rate = g_zdt_uart_config.baud_rate;
    attr.data_bits = UART_DATA_BIT_8;
    attr.stop_bits = UART_STOP_BIT_1;
    attr.parity = UART_PARITY_NONE;

    pin_cfg.tx_pin = g_zdt_uart_config.tx_pin;
    pin_cfg.rx_pin = g_zdt_uart_config.rx_pin;
    pin_cfg.cts_pin = PIN_NONE;
    pin_cfg.rts_pin = PIN_NONE;

    uapi_uart_deinit(g_zdt_uart_config.uart_bus);
    ret = uapi_uart_init(g_zdt_uart_config.uart_bus, &pin_cfg, &attr, NULL, &g_uart_buffer_config);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    osal_printk(
        "[zdt uart] init OK, bus=%u, TX=GPIO%u, RX=GPIO%u, baud=%u, rs485=%u\r\n", (unsigned)g_zdt_uart_config.uart_bus,
        (unsigned)g_zdt_uart_config.tx_pin, (unsigned)g_zdt_uart_config.rx_pin, (unsigned)g_zdt_uart_config.baud_rate,
        g_zdt_uart_config.rs485_dir_enable ? 1U : 0U);
    return ERRCODE_SUCC;
}

void zdt_uart_flush_rx(uint32_t timeout_ms)
{
    uint8_t byte;
    uint32_t start_ms = uapi_systick_get_ms();

    while ((uint32_t)(uapi_systick_get_ms() - start_ms) < timeout_ms) {
        int32_t ret = uapi_uart_read(g_zdt_uart_config.uart_bus, &byte, 1, 1);
        if (ret <= 0) {
            break;
        }
    }
}

int32_t zdt_uart_read_exact(uint8_t *buffer, uint16_t length, uint32_t timeout_ms)
{
    uint16_t received = 0;
    uint32_t start_ms;

    if (buffer == NULL || length == 0U) {
        return 0;
    }

    start_ms = uapi_systick_get_ms();
    while (received < length) {
        uint32_t elapsed_ms = (uint32_t)(uapi_systick_get_ms() - start_ms);
        uint32_t slice_ms;
        int32_t ret;

        if (elapsed_ms >= timeout_ms) {
            break;
        }

        slice_ms = timeout_ms - elapsed_ms;
        if (slice_ms > ZDT_UART_READ_SLICE_MS) {
            slice_ms = ZDT_UART_READ_SLICE_MS;
        }

        ret = uapi_uart_read(g_zdt_uart_config.uart_bus, &buffer[received], (uint32_t)(length - received), slice_ms);
        if (ret > 0) {
            received += (uint16_t)ret;
        } else {
            osal_msleep(1);
        }
    }

    return (int32_t)received;
}

int32_t zdt_uart_read_frame(uint8_t *buffer, uint16_t max_length, uint32_t first_byte_timeout_ms, uint32_t idle_gap_ms)
{
    uint16_t received = 0;
    uint32_t start_ms;
    uint32_t last_rx_ms = 0;

    if (buffer == NULL || max_length == 0U) {
        return 0;
    }

    if (idle_gap_ms == 0U) {
        idle_gap_ms = 1U;
    }

    start_ms = uapi_systick_get_ms();
    while (received < max_length) {
        uint32_t now_ms = uapi_systick_get_ms();
        uint32_t wait_budget_ms;
        uint32_t slice_ms;
        int32_t ret;

        if (received == 0U) {
            uint32_t elapsed_ms = (uint32_t)(now_ms - start_ms);
            if (elapsed_ms >= first_byte_timeout_ms) {
                break;
            }
            wait_budget_ms = first_byte_timeout_ms - elapsed_ms;
        } else {
            uint32_t idle_elapsed_ms = (uint32_t)(now_ms - last_rx_ms);
            if (idle_elapsed_ms >= idle_gap_ms) {
                break;
            }
            wait_budget_ms = idle_gap_ms - idle_elapsed_ms;
        }

        slice_ms = wait_budget_ms;
        if (slice_ms > ZDT_UART_READ_SLICE_MS) {
            slice_ms = ZDT_UART_READ_SLICE_MS;
        }
        if (slice_ms == 0U) {
            slice_ms = 1U;
        }

        ret = uapi_uart_read(g_zdt_uart_config.uart_bus, &buffer[received], (uint32_t)(max_length - received), slice_ms);
        if (ret > 0) {
            received += (uint16_t)ret;
            last_rx_ms = uapi_systick_get_ms();
        } else {
            osal_msleep(1);
        }
    }

    return (int32_t)received;
}

errcode_t zdt_uart_write_frame(const uint8_t *buffer, uint16_t length)
{
    int32_t ret;

    if (buffer == NULL || length == 0U) {
        return ERRCODE_INVALID_PARAM;
    }

    zdt_uart_set_rs485_direction(true);
    ret = uapi_uart_write(g_zdt_uart_config.uart_bus, buffer, length, 0);
    osal_udelay(zdt_uart_tx_settle_delay_us(length));
    zdt_uart_set_rs485_direction(false);

    if (ret != (int32_t)length) {
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCC;
}

errcode_t zdt_uart_transact(
    const uint8_t *tx_buffer, uint16_t tx_length, uint8_t *rx_buffer, uint16_t rx_length, uint32_t timeout_ms)
{
    errcode_t ret;
    int32_t rx_ret;

    if (tx_buffer == NULL || tx_length == 0U || rx_buffer == NULL || rx_length == 0U) {
        return ERRCODE_INVALID_PARAM;
    }

    zdt_uart_flush_rx(5);

    ret = zdt_uart_write_frame(tx_buffer, tx_length);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    rx_ret = zdt_uart_read_exact(rx_buffer, rx_length, timeout_ms);
    if (rx_ret != (int32_t)rx_length) {
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCC;
}

errcode_t zdt_uart_transact_frame(
    const uint8_t *tx_buffer, uint16_t tx_length, uint8_t *rx_buffer, uint16_t rx_capacity, uint16_t *rx_length,
    uint32_t first_byte_timeout_ms, uint32_t idle_gap_ms)
{
    errcode_t ret;
    int32_t rx_ret;

    if (tx_buffer == NULL || tx_length == 0U || rx_buffer == NULL || rx_capacity == 0U || rx_length == NULL) {
        return ERRCODE_INVALID_PARAM;
    }

    *rx_length = 0U;
    zdt_uart_flush_rx(5);

    ret = zdt_uart_write_frame(tx_buffer, tx_length);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    rx_ret = zdt_uart_read_frame(rx_buffer, rx_capacity, first_byte_timeout_ms, idle_gap_ms);
    if (rx_ret <= 0) {
        return ERRCODE_FAIL;
    }

    *rx_length = (uint16_t)rx_ret;
    return ERRCODE_SUCC;
}
