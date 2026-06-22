/**
 * @file ili9341_lcd.c
 * @brief ILI9341V SPI LCD driver for MSP3223 (240x320, IPS).
 *
 * Initialization sequence from vendor ILI9341V_Init.txt.
 */
#include "ili9341_lcd.h"
#include "screen_board.h"
#include "screen_config.h"
#include "soc_osal.h"

#include <stddef.h>

#define ILI9341_CMD_SWRESET     0x01
#define ILI9341_CMD_DISPOFF     0x28
#define ILI9341_CMD_CASET       0x2A
#define ILI9341_CMD_RASET       0x2B
#define ILI9341_CMD_RAMWR       0x2C
#define ILI9341_CMD_MADCTL      0x36
#define ILI9341_CMD_COLMOD      0x3A

#define ILI9341_MADCTL_MY       0x80
#define ILI9341_MADCTL_MV       0x20
#define ILI9341_MADCTL_BGR      0x08

/* Keep each polling SPI transfer below screen_lcd_spi_write()'s timeout count. */
#define ILI9341_RAW_CHUNK_BYTES  128U

static uint16_t g_lcd_width = SCREEN_LCD_WIDTH;
static uint16_t g_lcd_height = SCREEN_LCD_HEIGHT;

static errcode_t ili9341_write_cmd(uint8_t cmd)
{
    errcode_t ret;
    screen_lcd_cs(false);
    screen_lcd_dc(false);
    ret = screen_lcd_spi_write(&cmd, 1);
    screen_lcd_cs(true);
    return ret;
}

static errcode_t ili9341_write_data(const uint8_t *data, uint32_t len)
{
    errcode_t ret;
    screen_lcd_cs(false);
    screen_lcd_dc(true);
    ret = screen_lcd_spi_write(data, len);
    screen_lcd_cs(true);
    return ret;
}

static errcode_t ili9341_write_reg1(uint8_t cmd, uint8_t data)
{
    errcode_t ret = ili9341_write_cmd(cmd);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    return ili9341_write_data(&data, 1);
}

static errcode_t ili9341_write_regn(uint8_t cmd, const uint8_t *data, uint32_t len)
{
    errcode_t ret = ili9341_write_cmd(cmd);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    return ili9341_write_data(data, len);
}

static errcode_t ili9341_init_sequence(void)
{
    errcode_t ret;

#define TRY(expr) do { ret = (expr); if (ret != ERRCODE_SUCC) { return ret; } } while (0)

    TRY(ili9341_write_cmd(ILI9341_CMD_SWRESET));
    screen_board_delay_ms(120);
    TRY(ili9341_write_cmd(ILI9341_CMD_DISPOFF));

    /* Power control B */
    TRY(ili9341_write_regn(0xCF, (uint8_t[]){0x00, 0xC1, 0x30}, 3));

    /* Power on sequence control */
    TRY(ili9341_write_regn(0xED, (uint8_t[]){0x64, 0x03, 0x12, 0x81}, 4));

    /* Driver timing control A */
    TRY(ili9341_write_regn(0xE8, (uint8_t[]){0x85, 0x00, 0x78}, 3));

    /* Power control A */
    TRY(ili9341_write_regn(0xCB, (uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5));

    /* Pump ratio control */
    TRY(ili9341_write_reg1(0xF7, 0x20));

    /* Driver timing control B */
    TRY(ili9341_write_regn(0xEA, (uint8_t[]){0x00, 0x00}, 2));

    /* Power control 1 (VRH) */
    TRY(ili9341_write_reg1(0xC0, 0x13));

    /* Power control 2 (SAP/BT) */
    TRY(ili9341_write_reg1(0xC1, 0x13));

    /* VCM control 1 */
    TRY(ili9341_write_regn(0xC5, (uint8_t[]){0x1C, 0x35}, 2));

    /* VCM control 2 */
    TRY(ili9341_write_reg1(0xC7, 0xC8));

    /* Display inversion ON (IPS panel) */
    TRY(ili9341_write_cmd(0x21));

    /* Memory access control (BGR) */
    TRY(ili9341_write_reg1(ILI9341_CMD_MADCTL, 0x08));

    /* Display function control */
    TRY(ili9341_write_regn(0xB6, (uint8_t[]){0x0A, 0xA2}, 2));

    /* Pixel format: 16bit RGB565 */
    TRY(ili9341_write_reg1(ILI9341_CMD_COLMOD, 0x55));

    /* Interface control (MCU) */
    TRY(ili9341_write_regn(0xF6, (uint8_t[]){0x01, 0x30}, 2));

    /* Frame rate control */
    TRY(ili9341_write_regn(0xB1, (uint8_t[]){0x00, 0x1B}, 2));

    /* 3Gamma function disable */
    TRY(ili9341_write_reg1(0xF2, 0x00));

    /* Gamma curve selected */
    TRY(ili9341_write_reg1(0x26, 0x01));

    /* Positive gamma correction */
    TRY(ili9341_write_regn(0xE0, (uint8_t[]){
        0x0F, 0x35, 0x31, 0x0B, 0x0E, 0x06, 0x49,
        0xA7, 0x33, 0x07, 0x0F, 0x03, 0x0C, 0x0A, 0x00
    }, 15));

    /* Negative gamma correction */
    TRY(ili9341_write_regn(0xE1, (uint8_t[]){
        0x00, 0x0A, 0x0F, 0x04, 0x11, 0x08, 0x36,
        0x58, 0x4D, 0x07, 0x10, 0x0C, 0x32, 0x34, 0x0F
    }, 15));

    /* Exit sleep */
    TRY(ili9341_write_cmd(0x11));
    screen_board_delay_ms(120);

    /* Display on */
    TRY(ili9341_write_cmd(0x29));

#undef TRY
    return ERRCODE_SUCC;
}

errcode_t ili9341_init(void)
{
    osal_printk("[LCD] controller=ILI9341 panel=MSP3223 res=240x320\r\n");
    osal_printk("[ILI9341] lcd reset pin=GPIO%d\r\n", SCREEN_LCD_RST_PIN);

    /* MSP3223 hardware reset sequence. */
    osal_printk("[ILI9341] lcd reset low\r\n");
    screen_lcd_rst(false);
    screen_board_delay_ms(100);

    osal_printk("[ILI9341] lcd reset high\r\n");
    screen_lcd_rst(true);
    screen_board_delay_ms(50);

    errcode_t ret = ili9341_init_sequence();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[ILI9341] init sequence FAILED (0x%x)\r\n", ret);
        return ret;
    }
    osal_printk("[ILI9341] init sequence ok\r\n");

    ret = ili9341_set_rotation(ILI9341_ROTATION_270);
    if (ret == ERRCODE_SUCC) {
        screen_lcd_bl(true);
        osal_printk("[ILI9341] backlight on\r\n");
    }
    return ret;
}

errcode_t ili9341_set_rotation(ili9341_rotation_t rotation)
{
    uint8_t madctl;
    switch (rotation) {
        case ILI9341_ROTATION_0:
            g_lcd_width = SCREEN_LCD_WIDTH;
            g_lcd_height = SCREEN_LCD_HEIGHT;
            madctl = 0x08; /* BGR */
            break;
        case ILI9341_ROTATION_90:
            g_lcd_width = SCREEN_LCD_HEIGHT;
            g_lcd_height = SCREEN_LCD_WIDTH;
            madctl = 0x68; /* BGR + MV + MX */
            break;
        case ILI9341_ROTATION_180:
            g_lcd_width = SCREEN_LCD_WIDTH;
            g_lcd_height = SCREEN_LCD_HEIGHT;
            madctl = 0xC8; /* BGR + MY + MX */
            break;
        case ILI9341_ROTATION_CCW_90:
        default:
            g_lcd_width = SCREEN_LCD_HEIGHT;
            g_lcd_height = SCREEN_LCD_WIDTH;
            madctl = ILI9341_MADCTL_MY | ILI9341_MADCTL_MV | ILI9341_MADCTL_BGR;
            break;
    }
    errcode_t ret = ili9341_write_reg1(ILI9341_CMD_MADCTL, madctl);
    if (ret == ERRCODE_SUCC && rotation == ILI9341_ROTATION_90) {
        osal_printk("[LCD] ILI9341 rotation=landscape madctl=0x%02X\r\n", madctl);
    }
    return ret;
}

uint16_t ili9341_width(void)
{
    return g_lcd_width;
}

uint16_t ili9341_height(void)
{
    return g_lcd_height;
}

errcode_t ili9341_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t data[4];
    if (x1 >= g_lcd_width || y1 >= g_lcd_height || x0 > x1 || y0 > y1) {
        return ERRCODE_FAIL;
    }

    data[0] = (uint8_t)(x0 >> 8);
    data[1] = (uint8_t)x0;
    data[2] = (uint8_t)(x1 >> 8);
    data[3] = (uint8_t)x1;
    errcode_t ret = ili9341_write_regn(ILI9341_CMD_CASET, data, sizeof(data));
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    data[0] = (uint8_t)(y0 >> 8);
    data[1] = (uint8_t)y0;
    data[2] = (uint8_t)(y1 >> 8);
    data[3] = (uint8_t)y1;
    ret = ili9341_write_regn(ILI9341_CMD_RASET, data, sizeof(data));
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    return ili9341_write_cmd(ILI9341_CMD_RAMWR);
}

errcode_t ili9341_write_pixels_rgb565(const uint16_t *pixels, uint32_t count)
{
    uint8_t line[128];
    uint32_t offset = 0;
    errcode_t ret = ERRCODE_SUCC;

    if (pixels == NULL || count == 0) {
        return ERRCODE_SUCC;
    }

    screen_lcd_cs(false);
    screen_lcd_dc(true);
    while (offset < count) {
        uint32_t chunk = count - offset;
        if (chunk > (sizeof(line) / 2)) {
            chunk = sizeof(line) / 2;
        }

        for (uint32_t i = 0; i < chunk; i++) {
            uint16_t color = pixels[offset + i];
            line[i * 2] = (uint8_t)color;
            line[i * 2 + 1] = (uint8_t)(color >> 8);
        }

        ret = screen_lcd_spi_write(line, chunk * 2);
        if (ret != ERRCODE_SUCC) {
            break;
        }
        offset += chunk;
    }
    screen_lcd_cs(true);

    return ret;
}

errcode_t ili9341_write_pixels_raw(const uint8_t *pixels, uint32_t byte_len)
{
    if (pixels == NULL || byte_len == 0) {
        return ERRCODE_SUCC;
    }

    errcode_t ret = ERRCODE_SUCC;
    uint32_t offset = 0;

    screen_lcd_cs(false);
    screen_lcd_dc(true);
    while (offset < byte_len) {
        uint32_t chunk = byte_len - offset;
        if (chunk > ILI9341_RAW_CHUNK_BYTES) {
            chunk = ILI9341_RAW_CHUNK_BYTES;
        }

        ret = screen_lcd_spi_write(pixels + offset, chunk);
        if (ret != ERRCODE_SUCC) {
            break;
        }
        offset += chunk;
    }
    screen_lcd_cs(true);

    return ret;
}

errcode_t ili9341_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
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

    errcode_t ret = ili9341_set_window(x, y, x + w - 1, y + h - 1);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    uint32_t pixels = (uint32_t)w * h;
    while (pixels > 0) {
        uint32_t chunk = pixels;
        if (chunk > (sizeof(line) / 2)) {
            chunk = sizeof(line) / 2;
        }
        ret = ili9341_write_data(line, chunk * 2);
        if (ret != ERRCODE_SUCC) {
            return ret;
        }
        pixels -= chunk;
    }
    return ERRCODE_SUCC;
}

errcode_t ili9341_clear(uint16_t color)
{
    return ili9341_fill_rect(0, 0, g_lcd_width, g_lcd_height, color);
}
