/**
 * @file rx_mode.h
 * @brief Unified RX mode state.
 */
#ifndef WS63_LASER_RX_MODE_H
#define WS63_LASER_RX_MODE_H

#include "rx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void rx_mode_init(void);
void rx_mode_set(rx_mode_t mode);
rx_mode_t rx_mode_get(void);
const char *rx_mode_name(rx_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_MODE_H */
