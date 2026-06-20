/**
 * @file rx_stream.h
 * @brief Shared byte-stream command parser for unified RX transports.
 */
#ifndef WS63_LASER_RX_STREAM_H
#define WS63_LASER_RX_STREAM_H

#include "rx_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void rx_stream_init(void);
void rx_stream_on_ready(rx_stream_src_t src);
void rx_stream_on_poll(rx_stream_src_t src);
void rx_stream_on_byte(rx_stream_src_t src, uint8_t byte);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_STREAM_H */
