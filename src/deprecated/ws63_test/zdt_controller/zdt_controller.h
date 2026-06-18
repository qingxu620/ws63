/**
 * @file zdt_controller.h
 * @brief ZDT 高层控制接口
 */
#ifndef WS63_ZDT_CONTROLLER_H
#define WS63_ZDT_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>
#include "zdt_protocol.h"
#include "zdt_uart.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t device_addr;
    uint32_t reply_timeout_ms;
} zdt_controller_config_t;

errcode_t zdt_controller_init(const zdt_uart_config_t *uart_config, const zdt_controller_config_t *controller_config);

errcode_t zdt_controller_enable_motor(bool enable, bool sync, uint8_t *status);
errcode_t zdt_controller_run_speed(
    zdt_direction_t direction, uint16_t speed_rpm, uint8_t accel_level, bool sync, uint8_t *status);
errcode_t zdt_controller_run_position(
    zdt_direction_t direction, uint16_t speed_rpm, uint8_t accel_level, uint32_t pulses,
    zdt_position_mode_t position_mode, bool sync, uint8_t *status);
errcode_t zdt_controller_stop_now(bool sync, uint8_t *status);
errcode_t zdt_controller_sync_start(uint8_t target_addr, uint8_t *status);

errcode_t zdt_controller_set_single_turn_zero(bool persist, uint8_t *status);
errcode_t zdt_controller_clear_position(uint8_t *status);
errcode_t zdt_controller_release_stall(uint8_t *status);
errcode_t zdt_controller_modify_control_mode(bool persist, zdt_control_mode_t control_mode, uint8_t *status);

errcode_t zdt_controller_trigger_home(zdt_home_mode_t mode, bool sync, uint8_t *status);
errcode_t zdt_controller_modify_home_params(bool persist, const zdt_home_params_t *home_params, uint8_t *status);
errcode_t zdt_controller_abort_home(uint8_t *status);

errcode_t zdt_controller_read_sys_params_raw(zdt_sys_param_t param, uint8_t *frame, uint16_t frame_size, uint16_t *frame_len);
errcode_t zdt_controller_read_version(zdt_version_info_t *version_info);
errcode_t zdt_controller_read_motor_status(uint8_t *flags);
errcode_t zdt_controller_read_home_status(uint8_t *flags);
errcode_t zdt_controller_read_real_speed(int16_t *speed_rpm);
errcode_t zdt_controller_read_real_position(int32_t *position_raw);
uint32_t zdt_controller_get_reply_timeout_ms(void);

double zdt_controller_position_raw_to_degrees(int32_t position_raw);

#ifdef __cplusplus
}
#endif

#endif
