/**
 * @file gcode_processor.h
 * @brief Convert LaserGRBL/Grbl G-code lines into local motion commands.
 */
#ifndef GCODE_PROCESSOR_H
#define GCODE_PROCESSOR_H

#include "protocol.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void gcode_processor_init(void);
void gcode_processor_set_origin(void);
bool gcode_process_line(const char *line, int len, motion_cmd_t *out_cmds, int max_cmds, int *out_count);
void gcode_processor_build_emergency_stop(motion_cmd_t *out_cmd);
unsigned long gcode_processor_get_line_count(void);
double gcode_processor_get_feed_rate(void);
double gcode_processor_get_laser_power(void);
bool gcode_processor_laser_is_enabled(void);
bool gcode_processor_is_absolute_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* GCODE_PROCESSOR_H */
