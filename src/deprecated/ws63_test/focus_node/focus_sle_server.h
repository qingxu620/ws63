/**
 * @file focus_sle_server.h
 * @brief 感知与对焦节点 SLE Server
 */
#ifndef WS63_FOCUS_SLE_SERVER_H
#define WS63_FOCUS_SLE_SERVER_H

#include <stdint.h>
#include "errcode.h"
#include "focus_service.h"

#ifdef __cplusplus
extern "C" {
#endif

errcode_t focus_sle_server_init(void);
errcode_t focus_sle_server_publish_state(const focus_service_state_t *state);
uint16_t focus_sle_server_get_conn_id(void);

#ifdef __cplusplus
}
#endif

#endif
