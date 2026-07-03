/**
 * @file panel_rx_commands.h
 * @brief Screen-originated RX control commands using the SLE Job packet protocol.
 */
#ifndef PANEL_RX_COMMANDS_H
#define PANEL_RX_COMMANDS_H

#include "errcode.h"
#include <stdint.h>

errcode_t panel_rx_commands_init(void);
errcode_t panel_rx_commands_request_exec_stop(void);
errcode_t panel_rx_commands_request_abort(void);
errcode_t panel_rx_commands_request_focus_on(uint8_t power);
errcode_t panel_rx_commands_request_focus_off(void);
errcode_t panel_rx_commands_request_status(void);

#endif /* PANEL_RX_COMMANDS_H */
