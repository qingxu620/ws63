/**
 * @file focus_protocol.h
 * @brief 感知与对焦节点协议定义（供后续 SLE 任务级控制使用）
 */
#ifndef WS63_FOCUS_PROTOCOL_H
#define WS63_FOCUS_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FOCUS_PROTOCOL_VERSION 0x01U

/* ================= SLE UUID 定义 ================= */
#define SLE_FOCUS_SERVICE_UUID 0x1B0BU
#define SLE_FOCUS_CMD_CHAR_UUID 0x1B01U
#define SLE_FOCUS_STATUS_CHAR_UUID 0x1B02U

/* ================= SLE 属性 ================= */
#define SLE_FOCUS_PROPERTIES (0x01 | 0x02) /* READ | WRITE */
#define SLE_FOCUS_DESCRIPTOR (0x01 | 0x02) /* READ | WRITE */

typedef enum {
    FOCUS_CMD_QUERY_STATUS = 0x01,
    FOCUS_CMD_HOME_Z = 0x02,
    FOCUS_CMD_MOVE_Z_REL = 0x03,
    FOCUS_CMD_MOVE_Z_ABS = 0x04,
    FOCUS_CMD_STOP_Z = 0x05,
    FOCUS_CMD_READ_NFC = 0x10,
    FOCUS_CMD_MEASURE_HEIGHT = 0x11,
    FOCUS_CMD_AUTOFOCUS = 0x12,
} focus_cmd_type_t;

typedef enum {
    FOCUS_STATUS_IDLE = 0x00,
    FOCUS_STATUS_BUSY = 0x01,
    FOCUS_STATUS_ERROR = 0x02,
} focus_status_code_t;

typedef enum {
    FOCUS_ERR_NONE = 0x00,
    FOCUS_ERR_Z_NOT_READY = 0x01,
    FOCUS_ERR_Z_MOVE_REJECTED = 0x02,
    FOCUS_ERR_HOME_REJECTED = 0x03,
    FOCUS_ERR_HEIGHT_NOT_READY = 0x04,
    FOCUS_ERR_NFC_NOT_READY = 0x05,
    FOCUS_ERR_NOT_SUPPORTED = 0x06,
} focus_error_code_t;

#define FOCUS_FLAG_Z_LINK_READY 0x01U
#define FOCUS_FLAG_Z_ENABLED 0x02U
#define FOCUS_FLAG_Z_IN_POSITION 0x04U
#define FOCUS_FLAG_Z_HOMED 0x08U
#define FOCUS_FLAG_HEIGHT_READY 0x10U
#define FOCUS_FLAG_NFC_READY 0x20U

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t cmd;
    uint16_t seq;
    int32_t target_pulses;
    uint16_t speed_rpm;
    uint8_t accel_level;
    uint8_t reserved;
    uint16_t crc16;
} focus_node_cmd_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t status;
    uint8_t error_code;
    uint8_t flags;
    uint16_t ack_seq;
    int32_t z_position_pulses;
    int16_t z_speed_rpm;
    int32_t height_raw;
    uint16_t crc16;
} focus_node_status_t;

#ifdef __cplusplus
}
#endif

#endif
