/**
 * @file st7796_lcd.c
 * @brief ST7796S SPI LCD driver ported from the vendor MSP4030/MSP4031 demo.
 */
#include "st7796_lcd.h"
#include "screen_board.h"
#include "screen_config.h"
#include "soc_osal.h"

#include <stddef.h>

#define ST7796_CMD_CASET        0x2A
#define ST7796_CMD_RASET        0x2B
#define ST7796_CMD_RAMWR        0x2C
#define ST7796_CMD_MADCTL       0x36
#define ST7796_CMD_COLMOD       0x3A

static uint16_t g_lcd_width = SCREEN_LCD_WIDTH;
static uint16_t g_lcd_height = SCREEN_LCD_HEIGHT;

static errcode_t st7796_write_cmd(uint8_t cmd)
{
    errcode_t ret;
    screen_lcd_cs(false);
    screen_lcd_dc(false);
    ret = screen_lcd_spi_write(&cmd, 1);
    screen_lcd_cs(true);
    return ret;
}

static errcode_t st7796_write_data(const uint8_t *data, uint32_t len)
{
    errcode_t ret;
    screen_lcd_cs(false);
    screen_lcd_dc(true);
    ret = screen_lcd_spi_write(data, len);
    screen_lcd_cs(true);
    return ret;
}

static errcode_t st7796_write_reg1(uint8_t cmd, uint8_t data)
{
    errcode_t ret = st7796_write_cmd(cmd);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    return st7796_write_data(&data, 1);
}

static errcode_t st7796_write_regn(uint8_t cmd, const uint8_t *data, uint32_t len)
{
    errcode_t ret = st7796_write_cmd(cmd);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    return st7796_write_data(data, len);
}

static errcode_t st7796_init_sequence(void)
{
    static const uint8_t b6[] = {0x00, 0x02};
    static const uint8_t b5[] = {0x02, 0x03, 0x00, 0x04};
    static const uint8_t b1[] = {0x80, 0x10};
    static const uint8_t e8[] = {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33};
    static const uint8_t e0[] = {
        0xF0, 0x09, 0x13, 0x12, 0x12, 0x2B, 0x3C,
        0x44, 0x4B, 0x1B, 0x18, 0x17, 0x1D, 0x21
    };
    static const uint8_t e1[] = {
        0xF0, 0x09, 0x13, 0x0C, 0x0D, 0x27, 0x3B,
        0x44, 0x4D, 0x0B, 0x17, 0x17, 0x1D, 0x21
    };

    errcode_t ret;

#define TRY(expr) do { ret = (expr); if (ret != ERRCODE_SUCC) { return ret; } } while (0)
    TRY(st7796_write_reg1(0xF0, 0xC3));
    TRY(st7796_write_reg1(0xF0, 0x96));
    TRY(st7796_write_reg1(ST7796_CMD_MADCTL, 0x48));
    TRY(st7796_write_reg1(ST7796_CMD_COLMOD, 0x05));
    TRY(st7796_write_reg1(0xB0, 0x80));
    TRY(st7796_write_regn(0xB6, b6, sizeof(b6)));
    TRY(st7796_write_regn(0xB5, b5, sizeof(b5)));
    TRY(st7796_write_regn(0xB1, b1, sizeof(b1)));
    TRY(st7796_write_reg1(0xB4, 0x00));
    TRY(st7796_write_reg1(0xB7, 0xC6));
    TRY(st7796_write_reg1(0xC5, 0x1C));
    TRY(st7796_write_reg1(0xE4, 0x31));
    TRY(st7796_write_regn(0xE8, e8, sizeof(e8)));
    TRY(st7796_write_cmd(0xC2));
    TRY(st7796_write_cmd(0xA7));
    TRY(st7796_write_regn(0xE0, e0, sizeof(e0)));
    TRY(st7796_write_regn(0xE1, e1, sizeof(e1)));
    TRY(st7796_write_reg1(0xF0, 0x3C));
    TRY(st7796_write_reg1(0xF0, 0x69));
    TRY(st7796_write_cmd(0x13));
    TRY(st7796_write_cmd(0x11));
    screen_board_delay_ms(120);
    TRY(st7796_write_cmd(0x29));
#undef TRY
    return ERRCODE_SUCC;
}

errcode_t st7796_init(void)
{
    osal_printk("[SCREEN] lcd reset pin=GPIO%d active low\r\n", SCREEN_LCD_RST_PIN);

    osal_printk("[SCREEN] lcd reset low\r\n");
    screen_lcd_rst(false);
    screen_board_delay_ms(100);

    osal_printk("[SCREEN] lcd reset high\r\n");
    screen_lcd_rst(true);
    screen_board_delay_ms(50);

    errcode_t ret = st7796_init_sequence();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[LCD] init sequence FAILED (0x%x)\r\n", ret);
        return ret;
    }
    osal_printk("[LCD] init sequence ok\r\n");

    osal_printk("[SCREEN] sleep out 0x11\r\n");
    osal_printk("[SCREEN] display on 0x29\r\n");

    ret = st7796_set_rotation(ST7796_ROTATION_0);
    if (ret == ERRCODE_SUCC) {
        screen_lcd_bl(true);
        osal_printk("[LCD] backlight on\r\n");
    }
    return ret;
}

errcode_t st7796_set_rotation(st7796_rotation_t rotation)
{
    uint8_t madctl;
    switch (rotation) {
        case ST7796_ROTATION_0:
            g_lcd_width = SCREEN_LCD_WIDTH;
            g_lcd_height = SCREEN_LCD_HEIGHT;
            madctl = (1U << 3) | (1U << 6);
            break;
        case ST7796_ROTATION_90:
            g_lcd_width = SCREEN_LCD_HEIGHT;
            g_lcd_height = SCREEN_LCD_WIDTH;
            madctl = (1U << 3) | (1U << 5);
            break;
        case ST7796_ROTATION_180:
            g_lcd_width = SCREEN_LCD_WIDTH;
            g_lcd_height = SCREEN_LCD_HEIGHT;
            madctl = (1U << 3) | (1U << 7);
            break;
        case ST7796_ROTATION_270:
        default:
            g_lcd_width = SCREEN_LCD_HEIGHT;
            g_lcd_height = SCREEN_LCD_WIDTH;
            madctl = (1U << 3) | (1U << 7) | (1U << 6) | (1U << 5);
            break;
    }
    return st7796_write_reg1(ST7796_CMD_MADCTL, madctl);
}

uint16_t st7796_width(void)
{
    return g_lcd_width;
}

uint16_t st7796_height(void)
{
    return g_lcd_height;
}

errcode_t st7796_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t data[4];
    if (x1 >= g_lcd_width || y1 >= g_lcd_height || x0 > x1 || y0 > y1) {
        return ERRCODE_FAIL;
    }

    data[0] = (uint8_t)(x0 >> 8);
    data[1] = (uint8_t)x0;
    data[2] = (uint8_t)(x1 >> 8);
    data[3] = (uint8_t)x1;
    errcode_t ret = st7796_write_regn(ST7796_CMD_CASET, data, sizeof(data));
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    data[0] = (uint8_t)(y0 >> 8);
    data[1] = (uint8_t)y0;
    data[2] = (uint8_t)(y1 >> 8);
    data[3] = (uint8_t)y1;
    ret = st7796_write_regn(ST7796_CMD_RASET, data, sizeof(data));
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    return st7796_write_cmd(ST7796_CMD_RAMWR);
}

errcode_t st7796_write_pixels_rgb565(const uint16_t *pixels, uint32_t count)
{
    uint8_t line[128];
    uint32_t offset = 0;

    while (offset < count) {
        uint32_t chunk = count - offset;
        if (chunk > (sizeof(line) / 2)) {
            chunk = sizeof(line) / 2;
        }

        for (uint32_t i = 0; i < chunk; i++) {
            uint16_t color = pixels[offset + i];
            line[i * 2] = (uint8_t)(color >> 8);
            line[i * 2 + 1] = (uint8_t)color;
        }

        errcode_t ret = st7796_write_data(line, chunk * 2);
        if (ret != ERRCODE_SUCC) {
            return ret;
        }
        offset += chunk;
    }
    return ERRCODE_SUCC;
}

errcode_t st7796_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint8_t line[128];
    if (w == 0 || h == 0) {
        return ERRCODE_SUCC;
    }
    if ((uint32_t)x + w > g_lcd_width || (uint32_t)y + h > g_lcd_height) {
        return ERRCODE_FAIL;
    }

    for (uint32_t i = 0; i < sizeof(line) / 2; i++) {
        line[i * 2] = (uint8_t)(color >> 8);
        line[i * 2 + 1] = (uint8_t)color;
    }

    errcode_t ret = st7796_set_window(x, y, x + w - 1, y + h - 1);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    uint32_t pixels = (uint32_t)w * h;
    while (pixels > 0) {
        uint32_t chunk = pixels;
        if (chunk > (sizeof(line) / 2)) {
            chunk = sizeof(line) / 2;
        }
        ret = st7796_write_data(line, chunk * 2);
        if (ret != ERRCODE_SUCC) {
            return ret;
        }
        pixels -= chunk;
    }
    return ERRCODE_SUCC;
}

errcode_t st7796_clear(uint16_t color)
{
    return st7796_fill_rect(0, 0, g_lcd_width, g_lcd_height, color);
}
