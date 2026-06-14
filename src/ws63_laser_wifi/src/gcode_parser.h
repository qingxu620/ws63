/**
 * @file gcode_parser.h
 * @brief Small C G-code line parser.
 */
#ifndef GCODE_PARSER_H
#define GCODE_PARSER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GCODE_LINE_MAX 128

typedef struct {
    char line[GCODE_LINE_MAX];
    int len;
} gcode_line_t;

void gcode_init(gcode_line_t *gc);
void gcode_add_char(gcode_line_t *gc, char c);
bool gcode_parse(gcode_line_t *gc);
bool gcode_has_word(const gcode_line_t *gc, char letter);
double gcode_get_value(const gcode_line_t *gc, char letter);

#ifdef __cplusplus
}
#endif

#endif /* GCODE_PARSER_H */

