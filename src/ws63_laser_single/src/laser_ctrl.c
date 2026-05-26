/**
 * @file laser_ctrl.c
 * @brief Laser PWM control.
 */
#include "laser_ctrl.h"
#include "config.h"
#include "pinctrl.h"
#include "pwm.h"

#define LASER_PWM_PERIOD_TICKS 1000

static bool g_laser_enabled = false;
static uint16_t g_laser_power = 0;
static bool g_pwm_opened = false;

static void laser_pwm_stop(void)
{
#ifdef CONFIG_PWM_USING_V151
    uapi_pwm_stop_group(LASER_PWM_GROUP_ID);
#endif
    uapi_pwm_close(LASER_PWM_CHANNEL);
    g_pwm_opened = false;
}

static void laser_build_pwm_config(uint16_t power, pwm_config_t *cfg)
{
    uint32_t high_ticks = (uint32_t)((double)power / LASER_S_MAX * LASER_PWM_PERIOD_TICKS);

    if (high_ticks == 0) {
        high_ticks = 1;
    } else if (high_ticks >= LASER_PWM_PERIOD_TICKS) {
        high_ticks = LASER_PWM_PERIOD_TICKS - 1;
    }

    cfg->high_time = high_ticks;
    cfg->low_time = LASER_PWM_PERIOD_TICKS - high_ticks;
    cfg->offset_time = 0;
    cfg->cycles = 0;
    cfg->repeat = true;
}

errcode_t laser_ctrl_init(void)
{
    uapi_pin_set_mode(LASER_PWM_PIN, LASER_PWM_PIN_MODE);
    uapi_pwm_init();
    g_laser_enabled = false;
    g_laser_power = 0;
    laser_pwm_stop();
    return ERRCODE_SUCC;
}

void laser_set_power(uint16_t power)
{
    if (power > (uint16_t)LASER_S_MAX) {
        power = (uint16_t)LASER_S_MAX;
    }
    g_laser_power = power;

    if (!g_laser_enabled || power == 0) {
        laser_pwm_stop();
        return;
    }

    pwm_config_t cfg = {0};
    laser_build_pwm_config(power, &cfg);

    if (g_pwm_opened) {
#ifdef CONFIG_PWM_USING_V151
        uapi_pwm_update_cfg(LASER_PWM_CHANNEL, &cfg);
#else
        uapi_pwm_update_duty_ratio(LASER_PWM_CHANNEL, cfg.low_time, cfg.high_time);
#endif
        return;
    }

    if (uapi_pwm_open(LASER_PWM_CHANNEL, &cfg) != ERRCODE_SUCC) {
        return;
    }
    g_pwm_opened = true;

#ifdef CONFIG_PWM_USING_V151
    uint8_t channel_id = LASER_PWM_CHANNEL;
    uapi_pwm_set_group(LASER_PWM_GROUP_ID, &channel_id, 1);
    uapi_pwm_start_group(LASER_PWM_GROUP_ID);
#else
    uapi_pwm_start(LASER_PWM_CHANNEL);
#endif
}

void laser_enable(bool enable)
{
    g_laser_enabled = enable;
    if (enable) {
        laser_set_power(g_laser_power);
    } else {
        laser_pwm_stop();
    }
}

bool laser_is_enabled(void)
{
    return g_laser_enabled;
}

uint16_t laser_get_power(void)
{
    return g_laser_power;
}
