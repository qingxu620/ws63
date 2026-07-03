/**
 * @file panel_rx_commands.h
 * @brief Screen-originated RX control commands using the SLE Job packet protocol.
 */
#ifndef PANEL_RX_COMMANDS_H
#define PANEL_RX_COMMANDS_H

#include "errcode.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PANEL_RX_COMMAND_NONE = 0,
    PANEL_RX_COMMAND_EXEC_STOP,
    PANEL_RX_COMMAND_EXEC_RESUME,
    PANEL_RX_COMMAND_ABORT,
    PANEL_RX_COMMAND_FOCUS_ON,
    PANEL_RX_COMMAND_FOCUS_OFF,
    PANEL_RX_COMMAND_STATUS,
} panel_rx_command_type_t;

typedef struct {
    panel_rx_command_type_t type;
    errcode_t ret;
} panel_rx_command_result_t;

errcode_t panel_rx_commands_init(void);
errcode_t panel_rx_commands_request_exec_stop(void);
errcode_t panel_rx_commands_request_exec_resume(void);
errcode_t panel_rx_commands_request_abort(void);
errcode_t panel_rx_commands_request_focus_on(uint8_t power);
errcode_t panel_rx_commands_request_focus_off(void);
errcode_t panel_rx_commands_request_status(void);
void panel_rx_commands_set_offline_upload_active(bool active);
bool panel_rx_commands_is_offline_upload_paused(void);
bool panel_rx_commands_has_pending(void);
panel_rx_command_result_t panel_rx_commands_dispatch_pending(void);

#endif /* PANEL_RX_COMMANDS_H */
