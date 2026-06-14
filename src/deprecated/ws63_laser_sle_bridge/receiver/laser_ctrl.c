/**
 * @file laser_ctrl.c
 * @brief Laser PWM control.
 */
#include "laser_ctrl.h"
#include "config.h"
#include "gpio.h"
#include "pinctrl.h"
#include "pwm.h"

#define LASER_PWM_MIN_PERIOD_TICKS 8U
#define LASER_PWM_INVALID_TICKS 0xFFFFFFFFU

static bool g_laser_enabled = false;
static uint16_t g_laser_power = 0;
static bool g_pwm_opened = false;
static uint32_t g_last_pwm_clk_hz = 0;
static uint32_t g_last_period_ticks = 0;
static uint32_t g_last_high_ticks = 0;
static uint32_t g_last_low_ticks = 0;
static uint16_t g_last_requested_power = 0;
static uint16_t g_last_effective_power = 0;
static uint32_t g_applied_high_ticks = LASER_PWM_INVALID_TICKS;
static uint32_t g_applied_low_ticks = LASER_PWM_INVALID_TICKS;

static void laser_pin_force_low(void)
{
    uapi_pin_set_mode(LASER_PWM_PIN, HAL_PIO_FUNC_GPIO);
    uapi_gpio_set_dir(LASER_PWM_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val(LASER_PWM_PIN, GPIO_LEVEL_LOW);
}

static void laser_pwm_close_and_low(void)
{
    if (g_pwm_opened) {
#ifdef CONFIG_PWM_USING_V151
        uapi_pwm_stop_group(LASER_PWM_GROUP_ID);
#endif
        uapi_pwm_close(LASER_PWM_CHANNEL);
        g_pwm_opened = false;
    }
    g_applied_high_ticks = LASER_PWM_INVALID_TICKS;
    g_applied_low_ticks = LASER_PWM_INVALID_TICKS;
    laser_pin_force_low();
}

static void laser_build_pwm_config(uint16_t power, pwm_config_t *cfg)
{
    uint32_t pwm_clk = uapi_pwm_get_frequency(LASER_PWM_CHANNEL);
    uint32_t period_ticks = (pwm_clk + (LASER_PWM_FREQ_HZ / 2U)) / LASER_PWM_FREQ_HZ;
    if (period_ticks < LASER_PWM_MIN_PERIOD_TICKS) {
        period_ticks = LASER_PWM_MIN_PERIOD_TICKS;
    }

    uint32_t high_ticks = (uint32_t)((double)power / LASER_S_MAX * period_ticks);

    if (power == 0) {
        high_ticks = 0;
    } else if (high_ticks == 0) {
        high_ticks = 1;
    } else if (high_ticks >= period_ticks) {
        high_ticks = period_ticks - 1;
    }

    cfg->high_time = high_ticks;
    cfg->low_time = period_ticks - high_ticks;
    cfg->offset_time = 0;
    cfg->cycles = 0;
    cfg->repeat = true;

    g_last_pwm_clk_hz = pwm_clk;
    g_last_period_ticks = period_ticks;
    g_last_high_ticks = cfg->high_time;
    g_last_low_ticks = cfg->low_time;
    g_last_effective_power = power;
}

static void laser_apply_pwm_power(uint16_t power)
{
    pwm_config_t cfg = {0};
    laser_build_pwm_config(power, &cfg);

    if (g_pwm_opened) {
        if (g_applied_high_ticks == cfg.high_time && g_applied_low_ticks == cfg.low_time) {
            return;
        }
#ifdef CONFIG_PWM_USING_V151
        uapi_pwm_update_cfg(LASER_PWM_CHANNEL, &cfg);
#else
        uapi_pwm_update_duty_ratio(LASER_PWM_CHANNEL, cfg.low_time, cfg.high_time);
#endif
        g_applied_high_ticks = cfg.high_time;
        g_applied_low_ticks = cfg.low_time;
        return;
    }

    if (power == 0) {
        laser_pin_force_low();
        return;
    }

    uapi_pin_set_mode(LASER_PWM_PIN, LASER_PWM_PIN_MODE);
    if (uapi_pwm_open(LASER_PWM_CHANNEL, &cfg) != ERRCODE_SUCC) {
        laser_pin_force_low();
        return;
    }
    g_pwm_opened = true;
    g_applied_high_ticks = cfg.high_time;
    g_applied_low_ticks = cfg.low_time;

#ifdef CONFIG_PWM_USING_V151
    uint8_t channel_id = LASER_PWM_CHANNEL;
    uapi_pwm_set_group(LASER_PWM_GROUP_ID, &channel_id, 1);
    uapi_pwm_start_group(LASER_PWM_GROUP_ID);
#else
    uapi_pwm_start(LASER_PWM_CHANNEL);
#endif
}

static void laser_update_output(void)
{
    uint16_t output_power = g_laser_enabled ? g_laser_power : 0;
    laser_apply_pwm_power(output_power);
}

errcode_t laser_ctrl_init(void)
{
    uapi_pwm_init();
    g_laser_enabled = false;
    g_laser_power = 0;
    g_pwm_opened = false;
    laser_force_off();
    return ERRCODE_SUCC;
}

void laser_set_power(uint16_t power)
{
    g_last_requested_power = power;
    if (power > (uint16_t)LASER_S_MAX) {
        power = (uint16_t)LASER_S_MAX;
    }
    if (power == g_laser_power) {
        uint16_t output_power = g_laser_enabled ? power : 0;
        if (output_power == 0 || g_pwm_opened) {
            return;
        }
    }
    g_laser_power = power;
    laser_update_output();
}

void laser_enable(bool enable)
{
    if (!enable) {
        laser_force_off();
        return;
    }

    if (enable == g_laser_enabled) {
        uint16_t output_power = enable ? g_laser_power : 0;
        if (output_power == 0 || g_pwm_opened) {
            return;
        }
    }
    g_laser_enabled = enable;
    laser_update_output();
}

void laser_force_off(void)
{
    g_laser_enabled = false;
    g_laser_power = 0;
    laser_pwm_close_and_low();
}

bool laser_is_enabled(void)
{
    return g_laser_enabled;
}

uint16_t laser_get_power(void)
{
    return g_laser_power;
}

bool laser_pwm_is_opened(void)
{
    return g_pwm_opened;
}

uint32_t laser_pwm_clock_hz(void)
{
    return g_last_pwm_clk_hz;
}

uint32_t laser_pwm_period_ticks(void)
{
    return g_last_period_ticks;
}

uint32_t laser_pwm_high_ticks(void)
{
    return g_last_high_ticks;
}

uint32_t laser_pwm_low_ticks(void)
{
    return g_last_low_ticks;
}

uint16_t laser_pwm_last_requested_power(void)
{
    return g_last_requested_power;
}

uint16_t laser_pwm_last_effective_power(void)
{
    return g_last_effective_power;
}
