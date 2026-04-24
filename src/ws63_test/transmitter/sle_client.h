/**
 * @file sle_client.h
 * @brief 发射板 SLE Client
 *        底层统一管理多节点 SLE 扫描/连接/发现/写入，
 *        当前上层业务封装了 LaserRX 与 FocusND 两个 peer。
 */
#ifndef SLE_CLIENT_H
#define SLE_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "errcode.h"
#include "protocol.h"
#include "focus_protocol.h"
#include "safety_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

errcode_t sle_laser_client_init(void);
uint8_t sle_client_get_connected_peer_count(void);
uint8_t sle_client_get_configured_peer_count(void);
/* 发送一帧运动命令到接收板 (内部走 SSAP write request) */
errcode_t sle_laser_client_send_cmd(const motion_cmd_t *cmd);
/* 物理链路是否已连接 */
bool sle_laser_client_is_connected(void);
/* 是否已具备心跳发送条件: 已连接 + 命令/状态句柄均有效 */
bool sle_laser_client_can_send_heartbeat(void);
/* 是否可发送业务命令: 已连接 + 命令/状态句柄均有效 + 已收到首个有效状态包 */
bool sle_laser_client_is_ready(void);
/* 调试状态: 服务句柄是否已发现完成 */
bool sle_laser_client_has_handles_ready(void);
/* 调试状态: 是否已收到首个有效状态包 */
bool sle_laser_client_has_status_rx(void);
/* 接收板最近一次上报的运行状态 */
uint8_t sle_laser_client_get_remote_status(void);
/* 获取接收板最近一次上报的状态与坐标快照 */
void sle_laser_client_get_feedback_snapshot(uint8_t *status, double *x, double *y);
/* 接收板回传的剩余队列空间(用于发射板流控) */
uint8_t sle_laser_client_get_queue_free(void);
/* 最近一次收到的 ACK 序号 */
uint16_t sle_laser_client_get_last_ack_seq(void);
/* 当前发现到的命令/状态特征句柄，仅用于调试 */
uint16_t sle_laser_client_get_cmd_handle(void);
uint16_t sle_laser_client_get_status_handle(void);
/* 当前 SSAP 写请求在途数量 */
uint16_t sle_laser_client_get_pending_writes(void);
uint32_t sle_laser_client_get_last_business_write_ms(void);
/* 写请求发送/确认统计 */
uint32_t sle_laser_client_get_write_req_count(void);
uint32_t sle_laser_client_get_write_cfm_ok_count(void);
uint32_t sle_laser_client_get_write_cfm_fail_count(void);
uint32_t sle_laser_client_get_write_submit_fail_count(void);

/* ================= 感知与对焦节点链路 ================= */
errcode_t sle_focus_client_send_cmd(const focus_node_cmd_t *cmd);
bool sle_focus_client_is_connected(void);
bool sle_focus_client_has_handles_ready(void);
bool sle_focus_client_has_status_rx(void);
bool sle_focus_client_is_ready(void);
void sle_focus_client_get_status_snapshot(focus_node_status_t *status);
uint16_t sle_focus_client_get_cmd_handle(void);
uint16_t sle_focus_client_get_status_handle(void);
uint16_t sle_focus_client_get_pending_writes(void);

/* ================= 安全终端节点链路 ================= */
errcode_t sle_safety_client_send_cmd(const safety_node_cmd_t *cmd);
errcode_t sle_safety_client_led_on(void);
errcode_t sle_safety_client_led_off(void);
bool sle_safety_client_is_connected(void);
bool sle_safety_client_has_handles_ready(void);
bool sle_safety_client_has_status_rx(void);
bool sle_safety_client_is_ready(void);
void sle_safety_client_get_status_snapshot(safety_node_status_t *status);
uint16_t sle_safety_client_get_cmd_handle(void);
uint16_t sle_safety_client_get_status_handle(void);
uint16_t sle_safety_client_get_pending_writes(void);

#ifdef __cplusplus
}
#endif

#endif /* SLE_CLIENT_H */
