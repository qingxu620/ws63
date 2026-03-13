/**
 * @file sle_server.h
 * @brief 接收板 SLE Server
 */
#ifndef SLE_SERVER_H
#define SLE_SERVER_H

#include <stdint.h>
#include "errcode.h"

#ifdef __cplusplus
extern "C" {
#endif

errcode_t sle_laser_server_init(void);
errcode_t sle_laser_server_send_status(const uint8_t *data, uint16_t len);
uint16_t sle_laser_server_get_conn_id(void);

void sle_server_enable_cbk(void);
void sle_server_announce_register_cbks(void);

#ifdef __cplusplus
}
#endif

#endif /* SLE_SERVER_H */
