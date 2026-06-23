/**
 * @file screen_config.h
 * @brief Board configuration for MSP3223 (ILI9341V + FT6336U) WS63 screen board.
 *
 * MSP3223: 3.2" IPS, 240x320, ILI9341V, FT6336U, 18-pin FPC
 */
#ifndef WS63_SCREEN_CONFIG_H
#define WS63_SCREEN_CONFIG_H

#include <stdint.h>

#define SCREEN_LCD_NATIVE_WIDTH        240
#define SCREEN_LCD_NATIVE_HEIGHT       320
#define SCREEN_LCD_WIDTH               SCREEN_LCD_NATIVE_WIDTH
#define SCREEN_LCD_HEIGHT              SCREEN_LCD_NATIVE_HEIGHT

/* ILI9341 hardware rotation produces the landscape LVGL coordinate space. */
#define SCREEN_LVGL_WIDTH              SCREEN_LCD_NATIVE_HEIGHT
#define SCREEN_LVGL_HEIGHT             SCREEN_LCD_NATIVE_WIDTH
#define SCREEN_LVGL_DRAW_BUF_LINES     48

/* Board revision selection */
#define SCREEN_BOARD_REV_FINAL_HW_I2C  1
#define SCREEN_BOARD_REV_FLYWIRE_HW_I2C 0

/*
 * LCD pin configuration (same for all revisions)
 *   LCD_CS   -> GPIO8
 *   LCD_RST  -> GPIO0
 *   LCD_DC   -> GPIO10
 *   LCD_MOSI -> GPIO9  (SPI0_MOSI)
 *   LCD_SCK  -> GPIO7  (SPI0_SCK)
 *   LCD_MISO -> GPIO11 (SPI0_MISO)
 *   LCD_BL   -> GPIO5
 */
#define SCREEN_LCD_SPI_BUS             0
#define SCREEN_LCD_SPI_BAUDRATE        32000000  /* 32MHz */
#define SCREEN_LCD_SPI_BUS_CLK         32000000
#define SCREEN_LCD_SPI_SCK_PIN         7
#define SCREEN_LCD_SPI_MOSI_PIN        9
#define SCREEN_LCD_SPI_MISO_PIN        11
#define SCREEN_LCD_SPI_PIN_MODE        3   /* SPI0 alternate function */

#define SCREEN_LCD_CS_PIN              8
#define SCREEN_LCD_DC_PIN              10
#define SCREEN_LCD_RST_PIN             0   /* GPIO0 = LCD_RST on final board */
#define SCREEN_LCD_BL_PIN              5

/* Touch configuration (FT6336U) */
#define SCREEN_TOUCH_I2C_ADDR          0x38
#define SCREEN_TOUCH_RST_PIN           12
#define SCREEN_TOUCH_INT_PIN           13

#if SCREEN_BOARD_REV_FINAL_HW_I2C
/* Final board: hardware I2C1 at 400kHz */
#define SCREEN_TOUCH_SCL_PIN           16
#define SCREEN_TOUCH_SDA_PIN           15
#define SCREEN_TOUCH_I2C_BUS_ID        1
#define SCREEN_TOUCH_I2C_BAUDRATE      400000
#define SCREEN_TOUCH_I2C_PIN_MODE      2   /* PIN_MODE_2 = I2C1 alternate function */
#elif SCREEN_BOARD_REV_FLYWIRE_HW_I2C
/* Flywire board: hardware I2C1 at 400kHz */
#define SCREEN_TOUCH_SCL_PIN           16
#define SCREEN_TOUCH_SDA_PIN           15
#define SCREEN_TOUCH_I2C_BUS_ID        1
#define SCREEN_TOUCH_I2C_BAUDRATE      400000
#define SCREEN_TOUCH_I2C_PIN_MODE      2
#else
/* Original board: software bit-bang I2C */
#define SCREEN_TOUCH_SCL_PIN           0
#define SCREEN_TOUCH_SDA_PIN           2
#define SCREEN_TOUCH_I2C_DELAY_US      50
#endif

/* SD card chip select, reserved only, active low */
#define SCREEN_SD_CS_PIN               14

/* LCD-only color test mode: 1 = skip touch, loop color fills; 0 = normal mode */
#define SCREEN_LCD_ONLY_COLOR_TEST     0

#define SCREEN_COLOR_BLACK             0x0000
#define SCREEN_COLOR_WHITE             0xFFFF
#define SCREEN_COLOR_RED               0xF800
#define SCREEN_COLOR_GREEN             0x07E0
#define SCREEN_COLOR_BLUE              0x001F
#define SCREEN_COLOR_YELLOW            0xFFE0
#define SCREEN_COLOR_CYAN              0x07FF

#endif
