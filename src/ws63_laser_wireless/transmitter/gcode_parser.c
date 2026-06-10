/**
 * @file gcode_parser.c
 * @brief Lightweight G-code line parser.
 */
#include "gcode_parser.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void gcode_init(gcode_line_t *gc)
{
    gc->len = 0;
    gc->line[0] = '\0';
}

void gcode_add_char(gcode_line_t *gc, char c)
{
    if (gc->len < GCODE_LINE_MAX - 1) {
        gc->line[gc->len++] = (char)toupper((unsigned char)c);
        gc->line[gc->len] = '\0';
    }
}

bool gcode_parse(gcode_line_t *gc)
{
    for (int i = 0; i < gc->len; i++) {
        if (gc->line[i] == ';' || gc->line[i] == '(') {
            gc->line[i] = '\0';
            gc->len = i;
            break;
        }
    }

    int start = 0;
    while (start < gc->len && gc->line[start] == ' ') {
        start++;
    }
    if (start > 0 && start < gc->len) {
        memmove(gc->line, gc->line + start, gc->len - start + 1);
        gc->len -= start;
    }
    while (gc->len > 0 && gc->line[gc->len - 1] == ' ') {
        gc->line[--gc->len] = '\0';
    }

    return gc->len > 0;
}

bool gcode_has_word(const gcode_line_t *gc, char letter)
{
    letter = (char)toupper((unsigned char)letter);
    for (int i = 0; i < gc->len; i++) {
        if (gc->line[i] == letter && (i == 0 || !isalpha((unsigned char)gc->line[i - 1]))) {
            return true;
        }
    }
    return false;
}

double gcode_get_value(const gcode_line_t *gc, char letter)
{
    letter = (char)toupper((unsigned char)letter);
    for (int i = 0; i < gc->len; i++) {
        if (gc->line[i] == letter && (i == 0 || !isalpha((unsigned char)gc->line[i - 1]))) {
            return strtod(&gc->line[i + 1], NULL);
        }
    }
    return 0.0;
}
