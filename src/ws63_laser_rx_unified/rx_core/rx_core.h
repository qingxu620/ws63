/**
 * @file rx_core.h
 * @brief Unified RX core skeleton.
 */
#ifndef WS63_LASER_RX_CORE_H
#define WS63_LASER_RX_CORE_H

#include "rx_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void rx_core_init(void);
void rx_core_on_stream_ready(rx_stream_src_t src);
void rx_core_on_stream_poll(rx_stream_src_t src);
void rx_core_on_stream_byte(rx_stream_src_t src, uint8_t byte);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_CORE_H */
