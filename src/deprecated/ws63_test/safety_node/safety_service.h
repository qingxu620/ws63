/**
 * @file safety_service.h
 * @brief 安全终端节点业务层（当前第一版：LED 控制）
 */
#ifndef WS63_SAFETY_SERVICE_H
#define WS63_SAFETY_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include "errcode.h"
#include "safety_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t led_pin;
    bool led_active_high;
    bool boot_led_on;
} safety_service_config_t;

typedef struct {
    bool led_ready;
    bool led_on;
    uint8_t status_code;
    uint8_t error_code;
} safety_service_state_t;

errcode_t safety_service_init(const safety_service_config_t *config);
errcode_t safety_service_set_led(bool on);
errcode_t safety_service_get_state(safety_service_state_t *state);

#ifdef __cplusplus
}
#endif

#endif
