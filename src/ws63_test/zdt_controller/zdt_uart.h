/**
 * @file zdt_uart.h
 * @brief ZDT 控制板 UART/RS485 传输层
 */
#ifndef WS63_ZDT_UART_H
#define WS63_ZDT_UART_H

#include <stdbool.h>
#include <stdint.h>
#include "errcode.h"
#include "pinctrl.h"
#include "uart.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uart_bus_t uart_bus;
    pin_t tx_pin;
    pin_t rx_pin;
    pin_mode_t pin_mode;
    uint32_t baud_rate;
    bool rs485_dir_enable;
    pin_t rs485_dir_pin;
    bool rs485_dir_active_high;
} zdt_uart_config_t;

errcode_t zdt_uart_init(const zdt_uart_config_t *config);
void zdt_uart_flush_rx(uint32_t timeout_ms);
int32_t zdt_uart_read_exact(uint8_t *buffer, uint16_t length, uint32_t timeout_ms);
int32_t zdt_uart_read_frame(uint8_t *buffer, uint16_t max_length, uint32_t first_byte_timeout_ms, uint32_t idle_gap_ms);
errcode_t zdt_uart_write_frame(const uint8_t *buffer, uint16_t length);
errcode_t zdt_uart_transact(
    const uint8_t *tx_buffer, uint16_t tx_length, uint8_t *rx_buffer, uint16_t rx_length, uint32_t timeout_ms);
errcode_t zdt_uart_transact_frame(
    const uint8_t *tx_buffer, uint16_t tx_length, uint8_t *rx_buffer, uint16_t rx_capacity, uint16_t *rx_length,
    uint32_t first_byte_timeout_ms, uint32_t idle_gap_ms);

#ifdef __cplusplus
}
#endif

#endif
