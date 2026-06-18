/**
 * @file focus_service.h
 * @brief 感知与对焦节点业务层
 */
#ifndef WS63_FOCUS_SERVICE_H
#define WS63_FOCUS_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include "focus_protocol.h"
#include "zdt_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    zdt_uart_config_t zdt_uart;
    zdt_controller_config_t zdt_controller;
    bool boot_enable_z;
    bool boot_sync_zero;
} focus_service_config_t;

typedef struct {
    bool z_link_ready;
    bool z_enabled;
    bool z_in_position;
    bool z_homed;
    uint8_t motor_flags;
    uint8_t home_flags;
    int16_t z_speed_rpm;
    int32_t z_position_pulses;
    int32_t height_raw;
    uint8_t status_code;
    uint8_t error_code;
} focus_service_state_t;

errcode_t focus_service_init(const focus_service_config_t *config);
errcode_t focus_service_poll(void);
errcode_t focus_service_get_state(focus_service_state_t *state);

errcode_t focus_service_enable_z(bool enable);
errcode_t focus_service_stop_z(void);
errcode_t focus_service_move_z_abs_pulses(uint32_t target_pulses, uint16_t speed_rpm, uint8_t accel_level);
errcode_t focus_service_move_z_rel_pulses(
    zdt_direction_t direction, uint32_t delta_pulses, uint16_t speed_rpm, uint8_t accel_level);
errcode_t focus_service_home_z(zdt_home_mode_t home_mode);

errcode_t focus_service_measure_height(int32_t *height_raw);
errcode_t focus_service_autofocus(void);

#ifdef __cplusplus
}
#endif

#endif
