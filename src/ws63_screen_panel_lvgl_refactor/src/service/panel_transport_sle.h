/**
 * @file panel_transport_sle.h
 * @brief SLE transport for TX-mirrored status and offline Screen->RX jobs.
 */
#ifndef WS63_PANEL_TRANSPORT_SLE_H
#define WS63_PANEL_TRANSPORT_SLE_H

#include "errcode.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

errcode_t panel_transport_sle_start(void);
void panel_transport_sle_poll(void);
bool panel_transport_sle_rx_is_connected(void);
errcode_t panel_transport_sle_send_rx_packet(const void *data, uint16_t len);

typedef void (*panel_transport_rx_response_cb_t)(const uint8_t *data, uint16_t len);
void panel_transport_sle_set_rx_response_cb(panel_transport_rx_response_cb_t cb);
void panel_transport_sle_set_cmd_response_cb(panel_transport_rx_response_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* WS63_PANEL_TRANSPORT_SLE_H */
