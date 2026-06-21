/**
 * @file rx_types.h
 * @brief Shared RX core types for unified transport integration.
 */
#ifndef WS63_LASER_RX_TYPES_H
#define WS63_LASER_RX_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RX_MODE_NONE = 0,
    RX_MODE_GRBL_STREAM,
    RX_MODE_SLE_JOB,

    /* Experimental Phase 2A prototype modes retained for source compatibility. */
    RX_MODE_IDLE,
    RX_MODE_UART_DIRECT,
    RX_MODE_WIFI_TCP,
    RX_MODE_ERROR,
} rx_mode_t;

typedef enum {
    RX_SRC_UART = 0,
} rx_stream_src_t;

typedef struct {
    rx_mode_t mode;
    bool laser_on;
    bool motion_busy;
    uint16_t motion_queue_depth;
    uint32_t executed_count;
} rx_status_t;

#endif /* WS63_LASER_RX_TYPES_H */
