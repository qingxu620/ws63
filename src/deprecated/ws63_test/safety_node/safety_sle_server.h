/**
 * @file safety_sle_server.h
 * @brief 安全终端节点 SLE Server
 */
#ifndef WS63_SAFETY_SLE_SERVER_H
#define WS63_SAFETY_SLE_SERVER_H

#include <stdint.h>
#include "errcode.h"
#include "safety_service.h"

#ifdef __cplusplus
extern "C" {
#endif

errcode_t safety_sle_server_init(void);
errcode_t safety_sle_server_publish_state(const safety_service_state_t *state);
uint16_t safety_sle_server_get_conn_id(void);

#ifdef __cplusplus
}
#endif

#endif
