/**
 * @file protocol.h
 * @brief Local motion command format for the single-board sample.
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

#endif /* LASER_SINGLE_PROTOCOL_H */

