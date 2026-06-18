/**
 * @file uart_handler.h
 * @brief UART 处理器 — 接收上位机 G-Code 并回复 Grbl 协议
 */
#ifndef UART_HANDLER_H
#define UART_HANDLER_H

#include "errcode.h"

#ifdef __cplusplus
extern "C" {
#endif

errcode_t uart_handler_init(void);
/* UART 接收线程入口: 负责逐字节收包、按行解析和协议回复 */
int task_uart_rx_entry(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* UART_HANDLER_H */
