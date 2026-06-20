/**
 * @file gcode_parser.c
 * @brief Small C G-code line parser.
 */
#include "legacy_wifi_gcode_parser.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void legacy_wifi_gcode_init(legacy_wifi_gcode_line_t *gc)
{
    gc->len = 0;
    gc->line[0] = '\0';
}

void legacy_wifi_gcode_add_char(legacy_wifi_gcode_line_t *gc, char c)
{
    if (gc->len < LEGACY_WIFI_GCODE_LINE_MAX - 1) {
        gc->line[gc->len++] = (char)toupper((unsigned char)c);
        gc->line[gc->len] = '\0';
    }
}

bool legacy_wifi_gcode_parse(legacy_wifi_gcode_line_t *gc)
{
    for (int i = 0; i < gc->len; i++) {
        if (gc->line[i] == ';' || gc->line[i] == '(') {
            gc->line[i] = '\0';
            gc->len = i;
            break;
        }
    }

    int start = 0;
    while (start < gc->len && isspace((unsigned char)gc->line[start])) {
        start++;
    }
    if (start > 0 && start < gc->len) {
        memmove(gc->line, gc->line + start, gc->len - start + 1);
        gc->len -= start;
    }
    while (gc->len > 0 && isspace((unsigned char)gc->line[gc->len - 1])) {
        gc->line[--gc->len] = '\0';
    }

    return gc->len > 0;
}

bool legacy_wifi_gcode_has_word(const legacy_wifi_gcode_line_t *gc, char letter)
{
    letter = (char)toupper((unsigned char)letter);
    for (int i = 0; i < gc->len; i++) {
        if (gc->line[i] == letter && (i == 0 || !isalpha((unsigned char)gc->line[i - 1]))) {
            return true;
        }
    }
    return false;
}

double legacy_wifi_gcode_get_value(const legacy_wifi_gcode_line_t *gc, char letter)
{
    letter = (char)toupper((unsigned char)letter);
    for (int i = 0; i < gc->len; i++) {
        if (gc->line[i] == letter && (i == 0 || !isalpha((unsigned char)gc->line[i - 1]))) {
            return strtod(&gc->line[i + 1], NULL);
        }
    }
    return 0.0;
}

