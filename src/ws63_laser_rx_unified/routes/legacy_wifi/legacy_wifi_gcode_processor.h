/**
 * @file gcode_processor.h
 * @brief Convert LaserGRBL/Grbl G-code lines into local motion commands.
 */
#ifndef LEGACY_WIFI_GCODE_PROCESSOR_H
#define LEGACY_WIFI_GCODE_PROCESSOR_H

#include "legacy_wifi_motion_protocol.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void legacy_wifi_gcode_processor_init(void);
void legacy_wifi_gcode_processor_set_origin(void);
bool legacy_wifi_gcode_process_line(const char *line, int len, legacy_wifi_motion_cmd_t *out_cmds, int max_cmds, int *out_count);
void legacy_wifi_gcode_processor_build_emergency_stop(legacy_wifi_motion_cmd_t *out_cmd);
unsigned long legacy_wifi_gcode_processor_get_line_count(void);
double legacy_wifi_gcode_processor_get_feed_rate(void);
double legacy_wifi_gcode_processor_get_laser_power(void);
bool legacy_wifi_gcode_processor_laser_is_enabled(void);
bool legacy_wifi_gcode_processor_is_absolute_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* LEGACY_WIFI_GCODE_PROCESSOR_H */
