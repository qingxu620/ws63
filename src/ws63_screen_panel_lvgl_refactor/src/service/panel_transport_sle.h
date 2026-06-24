/**
 * @file panel_transport_sle.h
 * @brief SLE server receiving TX-mirrored panel status.
 */
#ifndef WS63_PANEL_TRANSPORT_SLE_H
#define WS63_PANEL_TRANSPORT_SLE_H

#include "errcode.h"

#ifdef __cplusplus
extern "C" {
#endif

errcode_t panel_transport_sle_start(void);

#ifdef __cplusplus
}
#endif

#endif /* WS63_PANEL_TRANSPORT_SLE_H */
