/**
 * @file gcode_parser.c
 * @brief 纯 C G-Code 解析器实现
 *        功能等效于 Arduino GCodeParser 库，但无 C++/String 依赖
 */
#include "gcode_parser.h"
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

void gcode_init(gcode_line_t *gc)
{
    /* 每次解析新行前都清空缓冲，避免残留字符干扰 */
    gc->len = 0;
    gc->line[0] = '\0';
}

void gcode_add_char(gcode_line_t *gc, char c)
{
    if (gc->len < GCODE_LINE_MAX - 1) {
        /* 统一转大写 */
        gc->line[gc->len++] = (char)toupper((unsigned char)c);
        gc->line[gc->len] = '\0';
    }
}

bool gcode_parse(gcode_line_t *gc)
{
    /* 去除注释: 分号或括号后的内容 */
    for (int i = 0; i < gc->len; i++) {
        if (gc->line[i] == ';' || gc->line[i] == '(') {
            gc->line[i] = '\0';
            gc->len = i;
            break;
        }
    }

    /* 去除首尾空白 */
    int start = 0;
    while (start < gc->len && gc->line[start] == ' ')
        start++;
    if (start > 0 && start < gc->len) {
        memmove(gc->line, gc->line + start, gc->len - start + 1);
        gc->len -= start;
    }
    while (gc->len > 0 && gc->line[gc->len - 1] == ' ') {
        gc->line[--gc->len] = '\0';
    }

    /* 空行、纯注释行会在这里被过滤掉 */
    return gc->len > 0;
}

bool gcode_has_word(const gcode_line_t *gc, char letter)
{
    letter = (char)toupper((unsigned char)letter);
    for (int i = 0; i < gc->len; i++) {
        /* 仅匹配“参数字母”位置，避免把上一个单词中间字符误判为参数 */
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
            /* 直接从字母后开始 strtod，支持整数/小数/符号 */
            return strtod(&gc->line[i + 1], NULL);
        }
    }
    return 0.0;
}
