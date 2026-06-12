/**
 * @file protocol.h
 * @brief Internal SLE motion protocol shared by transmitter and receiver.
 */
#ifndef WS63_SLE_LASER_PROTOCOL_H
#define WS63_SLE_LASER_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CMD_G0_MOVE 0x01
#define CMD_G1_MOVE 0x02
#define CMD_LASER_ON 0x03
#define CMD_LASER_OFF 0x04
#define CMD_SET_ORIGIN 0x05
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
#define STATUS_ERR_CRC 0x05

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

#ifdef __cplusplus
}
#endif

#endif /* WS63_SLE_LASER_PROTOCOL_H */
