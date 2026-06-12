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
 * Pin configuration for BearPi-Pico H3863 with 4.0inch SPI Touch Screen.
 * 
 * Screen Module Pin -> BearPi-Pico GPIO mapping:
 *   VCC     -> 3.3V power supply
 *   GND     -> Ground
 *   LCD_CS  -> GPIO8 (active low)
 *   LCD_RST -> GPIO6 (active low reset)
 *   LCD_RS  -> GPIO10 (high=data, low=command)
 *   SDI (MOSI) -> GPIO9 (SPI0_MOSI)
 *   SCK     -> GPIO7 (SPI0_SCK)
 *   LED     -> GPIO5 (backlight, high=on)
 *   SDO (MISO) -> GPIO11 (SPI0_MISO, optional)
 *   CTP_SCL -> GPIO0 (software I2C clock, needs 4.7kΩ pull-up)
 *   CTP_SDA -> GPIO2 (software I2C data, needs 4.7kΩ pull-up)
 *   CTP_RST -> GPIO12 (active low reset)
 *   CTP_INT -> GPIO13 (low when touched)
 */
#define SCREEN_LCD_SPI_BUS             0
#define SCREEN_LCD_SPI_FREQ_MHZ        10
#define SCREEN_LCD_SPI_BUS_CLK         32000000
#define SCREEN_LCD_SPI_SCK_PIN         7
#define SCREEN_LCD_SPI_MOSI_PIN        9
#define SCREEN_LCD_SPI_MISO_PIN        11
#define SCREEN_LCD_SPI_PIN_MODE        3   /* SPI0 alternate function */

#define SCREEN_LCD_CS_PIN              8
#define SCREEN_LCD_DC_PIN              10
#define SCREEN_LCD_RST_PIN             6
#define SCREEN_LCD_BL_PIN              5

#define SCREEN_TOUCH_I2C_ADDR          0x38
#define SCREEN_TOUCH_SCL_PIN           0
#define SCREEN_TOUCH_SDA_PIN           2
#define SCREEN_TOUCH_I2C_DELAY_US      5   /* bit-bang delay in microseconds */
#define SCREEN_TOUCH_RST_PIN           12
#define SCREEN_TOUCH_INT_PIN           13

#define SCREEN_COLOR_BLACK             0x0000
#define SCREEN_COLOR_WHITE             0xFFFF
#define SCREEN_COLOR_RED               0xF800
#define SCREEN_COLOR_GREEN             0x07E0
#define SCREEN_COLOR_BLUE              0x001F

#endif