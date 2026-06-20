/**
 * @file rx_motion_protocol.h
 * @brief Local motion command format for the unified RX skeleton.
 *
 * This is intentionally limited to executor-local types in Phase 1. It does not
 * replace or modify the SLE job packet protocol.
 */
#ifndef WS63_LASER_RX_MOTION_PROTOCOL_H
#define WS63_LASER_RX_MOTION_PROTOCOL_H

#include <stdint.h>

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

#endif /* WS63_LASER_RX_MOTION_PROTOCOL_H */
