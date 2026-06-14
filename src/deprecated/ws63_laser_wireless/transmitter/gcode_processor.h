/**
 * @file gcode_processor.h
 * @brief Convert LaserGRBL-compatible G-code into wireless motion packets.
 */
#ifndef LASER_WIRELESS_TX_GCODE_PROCESSOR_H
#define LASER_WIRELESS_TX_GCODE_PROCESSOR_H

#include "protocol.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void gcode_processor_init(void);
void gcode_processor_build_emergency_stop(motion_cmd_t *out_cmd);
bool gcode_process_line(const char *line, int len, motion_cmd_t *out_cmds, int max_cmds, int *out_count);
bool grbl_process_dollar(const char *line, char *response, int resp_size);
bool gcode_processor_is_idle(void);
void gcode_processor_update_feedback_pos(double x, double y);
void gcode_processor_get_feedback_pos(double *x, double *y);
void grbl_format_status(char *buf, int size, double cur_x, double cur_y, int is_idle);

#ifdef __cplusplus
}
#endif

#endif /* LASER_WIRELESS_TX_GCODE_PROCESSOR_H */
