/**
 * @file protocol.h
 * @brief Structured SLE job packet protocol and local motion command format.
 */
#ifndef WS63_LASER_SLE_JOB_PROTOCOL_H
#define WS63_LASER_SLE_JOB_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLE_JOB_PACKET_MAGIC 0xA55AU
#define SLE_JOB_PACKET_HEADER_LEN 10U
#define SLE_JOB_PACKET_MAX_PAYLOAD 224U
#define SLE_JOB_PACKET_MAX_SIZE (SLE_JOB_PACKET_HEADER_LEN + SLE_JOB_PACKET_MAX_PAYLOAD)

#define SLE_JOB_SERVICE_UUID 0x1B0B
#define SLE_JOB_DATA_CHAR_UUID 0x1B01
#define SLE_JOB_RESP_CHAR_UUID 0x1B02
#define SLE_JOB_RECEIVER_NAME "sle_job_rx"

#define SLE_PANEL_SERVICE_UUID 0x1B0C
#define SLE_PANEL_STATUS_CHAR_UUID 0x1B03
#define SLE_PANEL_SERVER_NAME "ws63_panel"

typedef enum {
    PKT_JOB_BEGIN = 0x01,
    PKT_JOB_DATA = 0x02,
    PKT_JOB_END = 0x03,
    PKT_JOB_ABORT = 0x04,

    PKT_EXEC_START = 0x10,
    PKT_EXEC_RESUME   = 0x11,
    PKT_EXEC_STOP     = 0x13,
    PKT_FOCUS_CTRL    = 0x14,
    PKT_ROUTE_SWITCH  = 0x15,

    PKT_STATUS_REQ    = 0x20,
    PKT_STATUS_RESP = 0x21,
    PKT_PANEL_STATUS = 0x22,

    PKT_ACK = 0x80,
    PKT_NACK = 0x81,
    PKT_CREDIT = 0x82,
} sle_job_packet_type_t;

typedef enum {
    JOB_STATE_IDLE = 0,
    JOB_STATE_RECEIVING_JOB = 1,
    JOB_STATE_JOB_READY = 2,
    JOB_STATE_EXECUTING = 3,
    JOB_STATE_PAUSED = 4,
    JOB_STATE_ABORTED = 5,
    JOB_STATE_ERROR = 6,
} sle_job_state_t;

typedef enum {
    JOB_STATUS_OK = 0,
    JOB_STATUS_BAD_MAGIC = 1,
    JOB_STATUS_BAD_CRC = 2,
    JOB_STATUS_BAD_SEQ = 3,
    JOB_STATUS_BAD_OFFSET = 4,
    JOB_STATUS_BAD_STATE = 5,
    JOB_STATUS_NO_SPACE = 6,
    JOB_STATUS_BAD_JOB = 7,
    JOB_STATUS_NOT_READY = 8,
    JOB_STATUS_INTERNAL_ERROR = 9,
} sle_job_status_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t type;
    uint8_t flags;
    uint16_t seq;
    uint16_t len;
    uint16_t crc16;
    uint8_t payload[];
} sle_packet_t;

typedef struct __attribute__((packed)) {
    uint32_t job_id;
    uint32_t total_size;
    uint16_t job_crc16;
    uint16_t reserved;
} job_begin_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t job_id;
    uint32_t offset;
    uint16_t data_len;
    uint8_t data[];
} job_data_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t job_id;
    uint32_t total_size;
    uint16_t job_crc16;
    uint16_t reserved;
} job_end_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t job_id;
} exec_start_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t on;
    uint8_t power;
} focus_ctrl_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t target_route;
    uint8_t flags;
    uint16_t reserved;
} route_switch_payload_t;

#define SLE_JOB_ROUTE_TARGET_NONE 0U
#define SLE_JOB_ROUTE_TARGET_LEGACY_UART 1U
#define SLE_JOB_ROUTE_TARGET_LEGACY_WIFI 2U
#define SLE_JOB_ROUTE_TARGET_SLE_JOB 3U
#define SLE_JOB_ROUTE_TARGET_SAFE 4U

typedef struct __attribute__((packed)) {
    uint8_t ack_type;
    uint8_t status;
    uint16_t ack_seq;
    uint32_t job_id;
    uint32_t offset;
    uint32_t credit;
} ack_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t state;
    uint8_t status;
    uint16_t last_seq;
    uint32_t job_id;
    uint32_t received_size;
    uint32_t total_size;
    uint32_t cache_free;
    uint32_t executed_lines;
} status_resp_payload_t;

#define PANEL_OWNER_NONE   0U
#define PANEL_OWNER_HOST   1U
#define PANEL_OWNER_SCREEN 2U

#define PANEL_MODE_IDLE      0U
#define PANEL_MODE_ONLINE    1U
#define PANEL_MODE_OFFLINE   2U
#define PANEL_MODE_ERROR     3U
#define PANEL_MODE_LINK_LOST 4U

#define PANEL_STATUS_FLAG_FOCUS_ACTIVE 0x01U
#define PANEL_STATUS_FLAG_LASER_ACTIVE 0x02U
#define PANEL_STATUS_FLAG_OWNER_LINK   0x04U
#define PANEL_STATUS_FLAG_ANY_LINK     0x08U

typedef struct __attribute__((packed)) {
    uint32_t seq;
    uint8_t owner;
    uint8_t mode;
    uint8_t job_state;
    uint8_t flags;
    uint32_t job_id;
    uint32_t received_size;
    uint32_t total_size;
    uint32_t executed_lines;
    uint32_t cache_free;
    uint32_t last_error;
    uint32_t tick_ms;
} panel_status_payload_t;

#define CMD_G0_MOVE 0x01
#define CMD_G1_MOVE 0x02
#define CMD_LASER_ON 0x03
#define CMD_LASER_OFF 0x04
#define CMD_SET_ORIGIN 0x05
#define CMD_EMERGENCY_STOP 0x07

#define FLAG_LASER_ON 0x01
#define FLAG_ABS_MODE 0x02

typedef struct {
    uint8_t cmd;
    uint8_t flags;
    uint16_t seq;
    float target_x;
    float target_y;
    float feed_rate;
    uint16_t laser_pwr;
} motion_cmd_t;

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_SLE_JOB_PROTOCOL_H */
