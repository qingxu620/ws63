/**
 * @file rx_status.h
 * @brief Unified RX status snapshot.
 */
#ifndef WS63_LASER_RX_STATUS_H
#define WS63_LASER_RX_STATUS_H

#include "rx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void rx_status_init(void);
void rx_status_get(rx_status_t *out_status);
void rx_status_print(void);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_STATUS_H */
