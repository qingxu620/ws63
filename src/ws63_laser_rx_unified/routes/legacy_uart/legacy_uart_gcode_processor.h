/**
 * @file legacy_uart_gcode_processor.h
 * @brief Convert LaserGRBL/Grbl G-code lines into local motion commands.
 */
#ifndef LEGACY_UART_GCODE_PROCESSOR_H
#define LEGACY_UART_GCODE_PROCESSOR_H

#include "legacy_uart_motion_protocol.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void legacy_uart_gcode_processor_init(void);
void legacy_uart_gcode_processor_set_origin(void);
bool legacy_uart_gcode_process_line(const char *line, int len, legacy_uart_motion_cmd_t *out_cmds, int max_cmds, int *out_count);
void legacy_uart_gcode_processor_build_emergency_stop(legacy_uart_motion_cmd_t *out_cmd);
unsigned long legacy_uart_gcode_processor_get_line_count(void);
double legacy_uart_gcode_processor_get_feed_rate(void);
double legacy_uart_gcode_processor_get_laser_power(void);
bool legacy_uart_gcode_processor_laser_is_enabled(void);
bool legacy_uart_gcode_processor_is_absolute_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* LEGACY_UART_GCODE_PROCESSOR_H */
