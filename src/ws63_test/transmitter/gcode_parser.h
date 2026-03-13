/**
 * @file gcode_parser.h
 * @brief 纯 C G-Code 解析器 — 替代 Arduino C++ GCodeParser 库
 */
#ifndef GCODE_PARSER_H
#define GCODE_PARSER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GCODE_LINE_MAX 128

/* 一行 G-Code 的轻量缓存:
 * line: 原始行(已转大写、已去注释前缀)
 * len : 当前有效长度(不含 '\0') */
typedef struct {
    char line[GCODE_LINE_MAX];
    int len;
} gcode_line_t;

/* 初始化行缓存 */
void gcode_init(gcode_line_t *gc);
/* 按字符写入并统一转大写，超长部分自动截断 */
void gcode_add_char(gcode_line_t *gc, char c);
/* 去注释/首尾空格，返回是否还有有效内容 */
bool gcode_parse(gcode_line_t *gc);
/* 判断是否包含指定字母参数(如 X/F/M/G/S) */
bool gcode_has_word(const gcode_line_t *gc, char letter);
/* 读取指定参数值，不存在时返回 0.0 */
double gcode_get_value(const gcode_line_t *gc, char letter);

#ifdef __cplusplus
}
#endif

#endif /* GCODE_PARSER_H */
