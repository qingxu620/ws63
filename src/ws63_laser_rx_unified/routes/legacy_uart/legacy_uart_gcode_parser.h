/**
 * @file legacy_uart_gcode_parser.h
 * @brief Small C G-code line parser.
 */
#ifndef LEGACY_UART_GCODE_PARSER_H
#define LEGACY_UART_GCODE_PARSER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEGACY_UART_GCODE_LINE_MAX 128

typedef struct {
    char line[LEGACY_UART_GCODE_LINE_MAX];
    int len;
} legacy_uart_gcode_line_t;

void legacy_uart_gcode_init(legacy_uart_gcode_line_t *gc);
void legacy_uart_gcode_add_char(legacy_uart_gcode_line_t *gc, char c);
bool legacy_uart_gcode_parse(legacy_uart_gcode_line_t *gc);
bool legacy_uart_gcode_has_word(const legacy_uart_gcode_line_t *gc, char letter);
double legacy_uart_gcode_get_value(const legacy_uart_gcode_line_t *gc, char letter);

#ifdef __cplusplus
}
#endif

#endif /* LEGACY_UART_GCODE_PARSER_H */
