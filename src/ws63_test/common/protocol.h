/**
 * @file protocol.h
 * @brief 星闪通信协议定义 — 发射板与接收板之间的数据包格式
 */
#ifndef LASER_PROTOCOL_H
#define LASER_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================= 命令类型 ================= */
#define CMD_G0_MOVE 0x01    /* 快速移动 (激光关) */
#define CMD_G1_MOVE 0x02    /* 直线插补 */
#define CMD_LASER_ON 0x03   /* M3/M4 开启激光 */
#define CMD_LASER_OFF 0x04  /* M5 关闭激光 */
#define CMD_SET_ORIGIN 0x05 /* G92 设置原点 */
#define CMD_SET_MODE 0x06   /* G90/G91 设置模式 */
#define CMD_HEARTBEAT 0xFE  /* 心跳 */

/* ================= 标志位 ================= */
#define FLAG_LASER_ON 0x01 /* bit0: 激光开关 */
#define FLAG_ABS_MODE 0x02 /* bit1: 绝对坐标模式 */

/* ================= 运动命令包 (发射板 → 接收板) ================= */
typedef struct __attribute__((packed)) {
    uint8_t cmd;        /* 命令类型 CMD_xxx */
    uint8_t flags;      /* 标志位 FLAG_xxx */
    uint16_t seq;       /* 序列号 */
    float target_x;     /* 目标 X (mm) */
    float target_y;     /* 目标 Y (mm) */
    float feed_rate;    /* 进给速度 (mm/min) */
    uint16_t laser_pwr; /* 激光功率 (0-1000) */
    uint16_t crc16;     /* CRC16 校验 */
} motion_cmd_t;         /* 20 bytes */

/* ================= 状态码 ================= */
#define STATUS_IDLE 0x00
#define STATUS_RUNNING 0x01
#define STATUS_ERROR 0x02

/* ================= 错误码 =================
 * 用于 status_pkt_t.error_code 字段 */
#define STATUS_ERR_NONE 0x00
#define STATUS_ERR_QUEUE_FULL 0x01
#define STATUS_ERR_INVALID_CMD 0x02
#define STATUS_ERR_INVALID_PARAM 0x03

/* ================= 状态反馈包 (接收板 → 发射板) ================= */
typedef struct __attribute__((packed)) {
    uint8_t status;     /* STATUS_xxx */
    uint8_t error_code; /* 错误码 */
    uint16_t ack_seq;   /* 接收板已接收(入队)的最新序列号 */
    uint8_t queue_free; /* 队列剩余空间 (0-32) */
    uint8_t reserved;
    uint16_t crc16; /* CRC16 校验 */
} status_pkt_t;     /* 8 bytes */

/* ================= 完整状态包 (低频发送) ================= */
typedef struct __attribute__((packed)) {
    status_pkt_t base;
    float cur_x;     /* 当前 X (mm) */
    float cur_y;     /* 当前 Y (mm) */
} status_full_pkt_t; /* 16 bytes */

/* ================= SLE UUID 定义 ================= */
#define SLE_LASER_SERVICE_UUID 0x1A0B
#define SLE_LASER_CMD_CHAR_UUID 0x1A01    /* 运动命令 characteristic */
#define SLE_LASER_STATUS_CHAR_UUID 0x1A02 /* 状态反馈 characteristic */

/* ================= SLE 属性 ================= */
#define SLE_LASER_PROPERTIES (0x01 | 0x02) /* READ | WRITE */
#define SLE_LASER_DESCRIPTOR (0x01 | 0x02) /* READ | WRITE */

#ifdef __cplusplus
}
#endif

#endif /* LASER_PROTOCOL_H */
