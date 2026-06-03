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

#ifdef __cplusplus
}
#endif

#endif /* LASER_CTRL_H */
