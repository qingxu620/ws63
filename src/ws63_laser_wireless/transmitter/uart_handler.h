/**
 * @file uart_handler.h
 * @brief LaserGRBL UART entry for the wireless transmitter.
 */
#ifndef LASER_WIRELESS_TX_UART_HANDLER_H
#define LASER_WIRELESS_TX_UART_HANDLER_H

#include "errcode.h"

#ifdef __cplusplus
extern "C" {
#endif

errcode_t uart_handler_init(void);
int task_uart_rx_entry(void *arg);
int task_tx_sender_entry(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* LASER_WIRELESS_TX_UART_HANDLER_H */
