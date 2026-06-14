/**
 * @file uart_handler.h
 * @brief LaserGRBL UART entry for the single-board sample.
 */
#ifndef UART_HANDLER_H
#define UART_HANDLER_H

#include "errcode.h"

#ifdef __cplusplus
extern "C" {
#endif

errcode_t uart_handler_init(void);
int task_uart_rx_entry(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* UART_HANDLER_H */

