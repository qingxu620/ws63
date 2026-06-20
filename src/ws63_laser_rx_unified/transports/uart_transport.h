/**
 * @file uart_transport.h
 * @brief USART Direct transport for unified RX.
 */
#ifndef WS63_LASER_RX_UART_TRANSPORT_H
#define WS63_LASER_RX_UART_TRANSPORT_H

#include "errcode.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

errcode_t uart_transport_init(void);
errcode_t uart_transport_start_task(void);
void uart_transport_write(const uint8_t *data, uint32_t len);
void uart_transport_write_str(const char *str);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_UART_TRANSPORT_H */
