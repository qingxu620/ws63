/**
 * @file st7796_text.c
 * @brief ASCII text rendering for the ST7796 LCD.
 */
#include "st7796_text.h"
#include "st7796_lcd.h"
#include "font_ascii.h"

#include <stddef.h>

int st7796_draw_char(uint16_t x, uint16_t y, char ch, uint16_t fg, uint16_t bg)
{
    if (ch < 32 || ch > 126) {
        return -1;
    }

    uint16_t lcd_w = st7796_width();
    uint16_t lcd_h = st7796_height();
    if (x + FONT_CHAR_WIDTH > lcd_w || y + FONT_CHAR_HEIGHT > lcd_h) {
        return -1;
    }

    const uint8_t *glyph = font_ascii_8x16[(int)ch - 32];
    uint16_t row_buf[FONT_CHAR_WIDTH];

    for (int row = 0; row < FONT_CHAR_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_CHAR_WIDTH; col++) {
            row_buf[col] = (bits & (1 << col)) ? fg : bg;
        }

        errcode_t ret = st7796_set_window(x, y + row, x + FONT_CHAR_WIDTH - 1, y + row);
        if (ret != ERRCODE_SUCC) {
            return -1;
        }
        ret = st7796_write_pixels_rgb565(row_buf, FONT_CHAR_WIDTH);
        if (ret != ERRCODE_SUCC) {
            return -1;
        }
    }

    return 0;
}

int st7796_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg)
{
    if (str == NULL) {
        return -1;
    }

    uint16_t lcd_w = st7796_width();
    uint16_t cx = x;
    int count = 0;

    while (*str) {
        if (cx + FONT_CHAR_WIDTH > lcd_w) {
            break;
        }
        if (st7796_draw_char(cx, y, *str, fg, bg) != 0) {
            break;
        }
        cx += FONT_CHAR_WIDTH;
        str++;
        count++;
    }

    return count;
}
