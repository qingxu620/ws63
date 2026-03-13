/**
 * @file laser_ctrl.h
 * @brief 激光 PWM 控制
 */
#ifndef LASER_CTRL_H
#define LASER_CTRL_H

#include <stdint.h>
#include <stdbool.h>
#include "errcode.h"

#ifdef __cplusplus
extern "C" {
#endif

errcode_t laser_ctrl_init(void);
void laser_set_power(uint16_t power); /* 0-1000 */
void laser_enable(bool enable);
bool laser_is_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* LASER_CTRL_H */
