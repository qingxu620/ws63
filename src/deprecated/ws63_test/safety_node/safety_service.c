/**
 * @file safety_service.c
 * @brief 安全终端节点业务层实现（当前第一版：LED 控制）
 */
#include "safety_service.h"

#include "common_def.h"
#include "gpio.h"
#include "hal_gpio.h"
#include "pinctrl.h"
#include "securec.h"

static safety_service_config_t g_safety_config = {0};
static safety_service_state_t g_safety_state = {
    .status_code = SAFETY_STATUS_ERROR,
    .error_code = SAFETY_ERR_LED_IO,
};
static bool g_safety_inited = false;

static gpio_level_t safety_led_level(bool on)
{
    bool high = on ? g_safety_config.led_active_high : !g_safety_config.led_active_high;
    return high ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW;
}

static void safety_state_set_error(uint8_t err)
{
    g_safety_state.status_code = (err == SAFETY_ERR_NONE) ? SAFETY_STATUS_IDLE : SAFETY_STATUS_ERROR;
    g_safety_state.error_code = err;
}

errcode_t safety_service_init(const safety_service_config_t *config)
{
    errcode_t ret;

    if (config == NULL) {
        return ERRCODE_INVALID_PARAM;
    }

    memcpy_s(&g_safety_config, sizeof(g_safety_config), config, sizeof(*config));
    uapi_pin_set_mode(g_safety_config.led_pin, HAL_PIO_FUNC_GPIO);
    ret = uapi_gpio_set_dir(g_safety_config.led_pin, GPIO_DIRECTION_OUTPUT);
    if (ret != ERRCODE_SUCC) {
        safety_state_set_error(SAFETY_ERR_LED_IO);
        g_safety_state.led_ready = false;
        return ret;
    }

    ret = uapi_gpio_set_val(g_safety_config.led_pin, safety_led_level(g_safety_config.boot_led_on));
    if (ret != ERRCODE_SUCC) {
        safety_state_set_error(SAFETY_ERR_LED_IO);
        g_safety_state.led_ready = false;
        return ret;
    }

    g_safety_inited = true;
    g_safety_state.led_ready = true;
    g_safety_state.led_on = g_safety_config.boot_led_on;
    safety_state_set_error(SAFETY_ERR_NONE);
    return ERRCODE_SUCC;
}

errcode_t safety_service_set_led(bool on)
{
    errcode_t ret;

    if (!g_safety_inited) {
        return ERRCODE_NOT_INIT;
    }

    ret = uapi_gpio_set_val(g_safety_config.led_pin, safety_led_level(on));
    if (ret != ERRCODE_SUCC) {
        g_safety_state.led_ready = false;
        safety_state_set_error(SAFETY_ERR_LED_IO);
        return ret;
    }

    g_safety_state.led_ready = true;
    g_safety_state.led_on = on;
    safety_state_set_error(SAFETY_ERR_NONE);
    return ERRCODE_SUCC;
}

errcode_t safety_service_get_state(safety_service_state_t *state)
{
    if (state == NULL) {
        return ERRCODE_INVALID_PARAM;
    }

    memcpy_s(state, sizeof(*state), &g_safety_state, sizeof(g_safety_state));
    return ERRCODE_SUCC;
}
