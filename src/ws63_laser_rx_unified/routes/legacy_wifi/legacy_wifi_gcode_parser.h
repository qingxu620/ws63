/**
 * @file gcode_parser.h
 * @brief Small C G-code line parser.
 */
#ifndef LEGACY_WIFI_GCODE_PARSER_H
#define LEGACY_WIFI_GCODE_PARSER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEGACY_WIFI_GCODE_LINE_MAX 128

typedef struct {
    char line[LEGACY_WIFI_GCODE_LINE_MAX];
    int len;
} legacy_wifi_gcode_line_t;

void legacy_wifi_gcode_init(legacy_wifi_gcode_line_t *gc);
void legacy_wifi_gcode_add_char(legacy_wifi_gcode_line_t *gc, char c);
bool legacy_wifi_gcode_parse(legacy_wifi_gcode_line_t *gc);
bool legacy_wifi_gcode_has_word(const legacy_wifi_gcode_line_t *gc, char letter);
double legacy_wifi_gcode_get_value(const legacy_wifi_gcode_line_t *gc, char letter);

#ifdef __cplusplus
}
#endif

#endif /* LEGACY_WIFI_GCODE_PARSER_H */

