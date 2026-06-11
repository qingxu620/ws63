/**
 * @file screen_config.h
 * @brief Board configuration for the ST7796 + FT6336 WS63 screen board.
 */
#ifndef WS63_SCREEN_CONFIG_H
#define WS63_SCREEN_CONFIG_H

#include <stdint.h>

#define SCREEN_LCD_WIDTH               320
#define SCREEN_LCD_HEIGHT              480

/*
 * Placeholder pins. Confirm the final pinmux against the actual WS63 board.
 * Keep this screen board independent from the laser single-board firmware.
 */
#define SCREEN_LCD_SPI_BUS             0
#define SCREEN_LCD_SPI_FREQ_MHZ        10
#define SCREEN_LCD_SPI_BUS_CLK         32000000
#define SCREEN_LCD_SPI_SCK_PIN         7
#define SCREEN_LCD_SPI_MOSI_PIN        9
#define SCREEN_LCD_SPI_MISO_PIN        8
#define SCREEN_LCD_SPI_PIN_MODE        1

#define SCREEN_LCD_CS_PIN              10
#define SCREEN_LCD_DC_PIN              11
#define SCREEN_LCD_RST_PIN             12
#define SCREEN_LCD_BL_PIN              13

#define SCREEN_TOUCH_I2C_BUS           1
#define SCREEN_TOUCH_I2C_BAUDRATE      400000
#define SCREEN_TOUCH_I2C_HSCODE        0
#define SCREEN_TOUCH_I2C_ADDR          0x38
#define SCREEN_TOUCH_SCL_PIN           14
#define SCREEN_TOUCH_SDA_PIN           15
#define SCREEN_TOUCH_I2C_PIN_MODE      1
#define SCREEN_TOUCH_RST_PIN           16
#define SCREEN_TOUCH_INT_PIN           17

#define SCREEN_COLOR_BLACK             0x0000
#define SCREEN_COLOR_WHITE             0xFFFF
#define SCREEN_COLOR_RED               0xF800
#define SCREEN_COLOR_GREEN             0x07E0
#define SCREEN_COLOR_BLUE              0x001F

#endif
