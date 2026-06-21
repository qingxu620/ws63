/**
 * @file sle_job_gcode_processor.h
 * @brief Convert LaserGRBL/Grbl G-code lines into local motion commands.
 */
#ifndef WS63_LASER_RX_UNIFIED_SLE_JOB_GCODE_PROCESSOR_H
#define WS63_LASER_RX_UNIFIED_SLE_JOB_GCODE_PROCESSOR_H

#include "sle_job_protocol.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void sle_job_gcode_processor_init(void);
void sle_job_gcode_processor_set_origin(void);
bool sle_job_gcode_process_line(const char *line, int len, sle_job_motion_cmd_t *out_cmds, int max_cmds, int *out_count);
void sle_job_gcode_processor_build_emergency_stop(sle_job_motion_cmd_t *out_cmd);
unsigned long sle_job_gcode_processor_get_line_count(void);
double sle_job_gcode_processor_get_feed_rate(void);
double sle_job_gcode_processor_get_laser_power(void);
bool sle_job_gcode_processor_laser_is_enabled(void);
bool sle_job_gcode_processor_is_absolute_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_UNIFIED_SLE_JOB_GCODE_PROCESSOR_H */
