/**
 * @file sle_server.h
 * @brief Receiver-side SLE server for the isolated wireless laser marker tree.
 */
#ifndef LASER_WIRELESS_SLE_SERVER_H
#define LASER_WIRELESS_SLE_SERVER_H

#include <stdint.h>
#include "errcode.h"

#ifdef __cplusplus
extern "C" {
#endif

errcode_t sle_laser_server_init(void);
errcode_t sle_laser_server_send_status(const uint8_t *data, uint16_t len);
uint16_t sle_laser_server_get_conn_id(void);
uint32_t sle_laser_server_get_heartbeat_rx_count(void);
uint32_t sle_laser_server_get_business_rx_count(void);
uint16_t sle_laser_server_get_last_ack_seq(void);

#ifdef __cplusplus
}
#endif

#endif /* LASER_WIRELESS_SLE_SERVER_H */
