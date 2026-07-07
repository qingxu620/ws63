/**
 * @file boot_policy.h
 * @brief RX SLE-first boot policy with WiFi coexist disabled by default.
 */
#ifndef WS63_LASER_RX_UNIFIED_BOOT_POLICY_H
#define WS63_LASER_RX_UNIFIED_BOOT_POLICY_H

#include "errcode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RX_BOOT_STATE_SLE_STARTING = 0,
    RX_BOOT_STATE_SLE_ADVERTISING,
    RX_BOOT_STATE_SLE_CONNECTED,
    RX_BOOT_STATE_SLE_WIFI_COEXIST,
    RX_BOOT_STATE_SAFE,
} rx_boot_state_t;

errcode_t rx_boot_policy_start(void);
rx_boot_state_t rx_boot_policy_get_state(void);
const char *rx_boot_policy_state_name(rx_boot_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_UNIFIED_BOOT_POLICY_H */
