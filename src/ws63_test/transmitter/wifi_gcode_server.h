/**
 * @file wifi_gcode_server.h
 * @brief 发射板 WiFi G-Code 入口
 *        基于官方 SoftAP 例程新增 TCP 文本命令入口，
 *        作为 UART 之外的第二条上游接入路径。
 */
#ifndef WIFI_GCODE_SERVER_H
#define WIFI_GCODE_SERVER_H

#include "errcode.h"

#ifdef __cplusplus
extern "C" {
#endif

errcode_t wifi_gcode_server_init(void);
/* WiFi 服务线程入口: 拉起 SoftAP 并监听 TCP G-Code 客户端 */
int task_wifi_gcode_entry(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_GCODE_SERVER_H */
