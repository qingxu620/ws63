/**
 * @file safety_protocol.h
 * @brief 安全终端节点协议定义（当前第一版：LED_ON / LED_OFF 验证链路）
 */
#ifndef WS63_SAFETY_PROTOCOL_H
#define WS63_SAFETY_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAFETY_PROTOCOL_VERSION 0x01U

/* ================= SLE UUID 定义 ================= */
#define SLE_SAFETY_SERVICE_UUID 0x1C0BU
#define SLE_SAFETY_CMD_CHAR_UUID 0x1C01U
#define SLE_SAFETY_STATUS_CHAR_UUID 0x1C02U

/* ================= SLE 属性 ================= */
#define SLE_SAFETY_PROPERTIES (0x01 | 0x02) /* READ | WRITE */
#define SLE_SAFETY_DESCRIPTOR (0x01 | 0x02) /* READ | WRITE */

typedef enum {
    SAFETY_CMD_LED_OFF = 0x01,
    SAFETY_CMD_LED_ON = 0x02,
} safety_cmd_type_t;

typedef enum {
    SAFETY_STATUS_IDLE = 0x00,
    SAFETY_STATUS_ERROR = 0x01,
} safety_status_code_t;

typedef enum {
    SAFETY_ERR_NONE = 0x00,
    SAFETY_ERR_LED_IO = 0x01,
    SAFETY_ERR_NOT_SUPPORTED = 0x02,
} safety_error_code_t;

#define SAFETY_FLAG_LED_READY 0x01U
#define SAFETY_FLAG_LED_ON 0x02U

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t cmd;
    uint16_t seq;
    uint32_t reserved;
    uint16_t crc16;
} safety_node_cmd_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t status;
    uint8_t error_code;
    uint8_t flags;
    uint16_t ack_seq;
    uint16_t crc16;
} safety_node_status_t;

#ifdef __cplusplus
}
#endif

#endif
