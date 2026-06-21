/**
 * @file sle_job_gcode_parser.h
 * @brief Small C G-code line parser.
 */
#ifndef WS63_LASER_RX_UNIFIED_SLE_JOB_GCODE_PARSER_H
#define WS63_LASER_RX_UNIFIED_SLE_JOB_GCODE_PARSER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLE_JOB_GCODE_LINE_MAX 128

typedef struct {
    char line[SLE_JOB_GCODE_LINE_MAX];
    int len;
} sle_job_gcode_line_t;

void sle_job_gcode_init(sle_job_gcode_line_t *gc);
void sle_job_gcode_add_char(sle_job_gcode_line_t *gc, char c);
bool sle_job_gcode_parse(sle_job_gcode_line_t *gc);
bool sle_job_gcode_has_word(const sle_job_gcode_line_t *gc, char letter);
double sle_job_gcode_get_value(const sle_job_gcode_line_t *gc, char letter);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_UNIFIED_SLE_JOB_GCODE_PARSER_H */
