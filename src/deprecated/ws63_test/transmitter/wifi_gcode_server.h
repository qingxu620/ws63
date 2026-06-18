/**
 * @file wifi_gcode_server.h
 * @brief 发射板 WiFi G-Code 入口
 *        基于官方 SoftAP / STA 例程新增 TCP 文本命令入口，
 *        作为 UART 之外的第二条上游接入路径。
 */
#ifndef WIFI_GCODE_SERVER_H
#define WIFI_GCODE_SERVER_H

#include "errcode.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_GCODE_STATUS_SSID_MAX 33
#define WIFI_GCODE_STATUS_IFNAME_MAX 16
#define WIFI_GCODE_STATUS_IP_MAX 16

typedef enum {
    WIFI_GCODE_MODE_SOFTAP = 0,
    WIFI_GCODE_MODE_STA = 1,
} wifi_gcode_mode_t;

typedef struct {
    bool event_registered;
    bool net_ready;
    bool tcp_listening;
    bool client_connected;
    bool sta_link_up;
    uint32_t softap_sta_count;
    int32_t last_disconnect_reason;
    uint16_t tcp_port;
    wifi_gcode_mode_t mode;
    char ssid[WIFI_GCODE_STATUS_SSID_MAX];
    char ifname[WIFI_GCODE_STATUS_IFNAME_MAX];
    char ip[WIFI_GCODE_STATUS_IP_MAX];
} wifi_gcode_status_snapshot_t;

errcode_t wifi_gcode_server_init(void);
/* WiFi 服务线程入口: 按配置拉起 SoftAP 或 STA，并监听 TCP G-Code 客户端 */
int task_wifi_gcode_entry(void *arg);
/* 对外暴露当前 WiFi 运行快照，方便后续网页/BLE/调试界面复用。 */
void wifi_gcode_server_get_status(wifi_gcode_status_snapshot_t *status);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_GCODE_SERVER_H */
