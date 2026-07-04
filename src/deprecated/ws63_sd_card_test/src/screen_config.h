/**
 * @file screen_config.h
 * @brief Minimal MSP3223 shared-SPI pin config for the SD-card isolation test.
 */
#ifndef WS63_SD_TEST_SCREEN_CONFIG_H
#define WS63_SD_TEST_SCREEN_CONFIG_H

/* Shared SPI0 pins on the MSP3223 screen board. LCD is not initialized here. */
#define SCREEN_LCD_SPI_BUS             0
#define SCREEN_LCD_SPI_BAUDRATE        1000000
#define SCREEN_LCD_SPI_BUS_CLK         80000000
#define SCREEN_LCD_SPI_SCK_PIN         7
#define SCREEN_LCD_SPI_MOSI_PIN        9
#define SCREEN_LCD_SPI_MISO_PIN        11
#define SCREEN_LCD_SPI_PIN_MODE        3

/* Keep the inactive LCD deselected while testing the SD card. */
#define SCREEN_LCD_CS_PIN              8
#define SCREEN_SD_CS_PIN               14
#define SCREEN_PANEL_ENABLE_SD         1

/* The isolation test must not depend on LCD-side uapi_spi_init(). */
#define SCREEN_SD_TEST_BITBANG_ONLY    1

#endif
