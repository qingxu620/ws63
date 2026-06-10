/**
 * @file protocol.h
 * @brief Wireless motion/status packet format shared by TX and RX.
 */
#ifndef LASER_SINGLE_PROTOCOL_H
#define LASER_SINGLE_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CMD_G0_MOVE 0x01
#define CMD_G1_MOVE 0x02
#define CMD_LASER_ON 0x03
#define CMD_LASER_OFF 0x04
#define CMD_SET_ORIGIN 0x05
#define CMD_SET_MODE 0x06
#define CMD_EMERGENCY_STOP 0x07
#define CMD_HEARTBEAT 0xFE

#define FLAG_LASER_ON 0x01
#define FLAG_ABS_MODE 0x02

typedef struct __attribute__((packed)) {
    uint8_t cmd;
    uint8_t flags;
    uint16_t seq;
    float target_x;
    float target_y;
    float feed_rate;
    uint16_t laser_pwr;
    uint16_t crc16;
} motion_cmd_t;

#define STATUS_IDLE 0x00
#define STATUS_RUNNING 0x01
#define STATUS_ERROR 0x02

#define STATUS_ERR_NONE 0x00
#define STATUS_ERR_QUEUE_FULL 0x01
#define STATUS_ERR_INVALID_CMD 0x02
#define STATUS_ERR_INVALID_PARAM 0x03
#define STATUS_ERR_ESTOP 0x04

typedef struct __attribute__((packed)) {
    uint8_t status;
    uint8_t error_code;
    uint16_t ack_seq;
    uint8_t queue_free;
    uint8_t reserved;
    uint16_t crc16;
} status_pkt_t;

typedef struct __attribute__((packed)) {
    status_pkt_t base;
    float cur_x;
    float cur_y;
} status_full_pkt_t;

#define SLE_LASER_SERVICE_UUID 0x1A0B
#define SLE_LASER_CMD_CHAR_UUID 0x1A01
#define SLE_LASER_STATUS_CHAR_UUID 0x1A02
#define SLE_LASER_PROPERTIES (0x01 | 0x02)
#define SLE_LASER_DESCRIPTOR (0x01 | 0x02)

#ifdef __cplusplus
}
#endif

#endif /* LASER_SINGLE_PROTOCOL_H */
