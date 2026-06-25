/**
 * @file laser_ctrl.h
 * @brief Laser PWM control.
 */
#ifndef LASER_CTRL_H
#define LASER_CTRL_H

#include "errcode.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

errcode_t laser_ctrl_init(void);
void laser_set_power(uint16_t power);
void laser_enable(bool enable);
void laser_force_off(void);
bool laser_is_enabled(void);
uint16_t laser_get_power(void);
bool laser_pwm_is_opened(void);
uint32_t laser_pwm_clock_hz(void);
uint32_t laser_pwm_period_ticks(void);
uint32_t laser_pwm_high_ticks(void);
uint32_t laser_pwm_low_ticks(void);
uint16_t laser_pwm_last_requested_power(void);
uint16_t laser_pwm_last_effective_power(void);

#ifdef __cplusplus
}
#endif

#endif /* LASER_CTRL_H */
