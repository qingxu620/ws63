/**
 * @file laser_ctrl.c
 * @brief 激光 PWM 控制实现
 */
#include "laser_ctrl.h"
#include "config.h"
#include "pinctrl.h"
#include "pwm.h"
#include "soc_osal.h"

#define LASER_PWM_PERIOD_US 1000 /* 1kHz → 周期 1000μs */

static bool g_laser_enabled = false;
static uint16_t g_laser_power = 0; /* 0-1000 */

errcode_t laser_ctrl_init(void)
{
    /* 配置 PWM 引脚复用: GPIO2 → PWM2 */
    uapi_pin_set_mode(LASER_PWM_PIN, LASER_PWM_PIN_MODE);

    /* 确保激光默认关闭 */
    g_laser_enabled = false;
    g_laser_power = 0;

    return ERRCODE_SUCC;
}

void laser_set_power(uint16_t power)
{
    if (power > 1000) {
        power = 1000;
    }
    g_laser_power = power;

    if (g_laser_enabled && power > 0) {
        uint32_t high_us = (uint32_t)((double)power / LASER_S_MAX * LASER_PWM_PERIOD_US);
        uint32_t low_us = LASER_PWM_PERIOD_US - high_us;

        if (high_us == 0)
            high_us = 1;
        if (low_us == 0)
            low_us = 1;

        uapi_pwm_close(LASER_PWM_CHANNEL);
        pwm_config_t cfg = {0};
        cfg.high_time = high_us;
        cfg.low_time = low_us;
        uapi_pwm_open(LASER_PWM_CHANNEL, &cfg);
        uapi_pwm_start(LASER_PWM_CHANNEL);
    }
}

void laser_enable(bool enable)
{
    g_laser_enabled = enable;
    if (enable) {
        laser_set_power(g_laser_power);
    } else {
        uapi_pwm_close(LASER_PWM_CHANNEL);
    }
}

bool laser_is_enabled(void)
{
    return g_laser_enabled;
}
