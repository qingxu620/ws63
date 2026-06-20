/**
 * @file legacy_uart_motion_protocol.h
 * @brief Local motion command format for the legacy UART route.
 */
#ifndef LEGACY_UART_MOTION_PROTOCOL_H
#define LEGACY_UART_MOTION_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEGACY_UART_CMD_G0_MOVE 0x01
#define LEGACY_UART_CMD_G1_MOVE 0x02
#define LEGACY_UART_CMD_LASER_ON 0x03
#define LEGACY_UART_CMD_LASER_OFF 0x04
#define LEGACY_UART_CMD_SET_ORIGIN 0x05
#define LEGACY_UART_CMD_EMERGENCY_STOP 0x07

#define LEGACY_UART_FLAG_LASER_ON 0x01
#define LEGACY_UART_FLAG_ABS_MODE 0x02

typedef struct {
    uint8_t cmd;
    uint8_t flags;
    uint16_t seq;
    float target_x;
    float target_y;
    float feed_rate;
    uint16_t laser_pwr;
} legacy_uart_motion_cmd_t;

#ifdef __cplusplus
}
#endif

#endif /* LEGACY_UART_MOTION_PROTOCOL_H */
