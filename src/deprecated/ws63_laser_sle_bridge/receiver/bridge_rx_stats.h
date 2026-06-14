/**
 * @file bridge_rx_stats.h
 * @brief RX-side bridge response counters for diagnostics.
 */
#ifndef BRIDGE_RX_STATS_H
#define BRIDGE_RX_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned long resp_generated;
    unsigned long notify_retry;
    unsigned long notify_fail;
    unsigned long max_resp_delay_ms;
} bridge_rx_stats_t;

void bridge_rx_stats_get(bridge_rx_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_RX_STATS_H */
