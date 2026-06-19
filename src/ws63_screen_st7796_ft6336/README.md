# WS63 ST7796 + FT6336 Screen Port

This directory is an isolated bring-up area for the
`4.0inch_SPI_Module_ST7796_MSP4030_MSP4031_V1.0` touch display module.

## Hardware Configuration (Final Board)

### LCD signals

| Function | GPIO | Notes |
|----------|------|-------|
| LCD_CS | GPIO8 | SPI0 chip select, active low |
| LCD_RST | GPIO0 | Active low reset |
| LCD_DC | GPIO10 | High=data, low=command |
| LCD_SCK | GPIO7 | SPI0 clock |
| LCD_MOSI | GPIO9 | SPI0 data out |
| LCD_MISO | GPIO11 | SPI0 data in (optional) |
| LCD_BL | GPIO5 | Backlight, high=on |

### Touch signals (hardware I2C1)

| Function | GPIO | Notes |
|----------|------|-------|
| CTP_SCL | GPIO16 | I2C1_SCL, PIN_MODE_2 |
| CTP_SDA | GPIO15 | I2C1_SDA, PIN_MODE_2 |
| CTP_RST | GPIO12 | Active low reset (tie to 3.3V if GPIO12 can't drive HIGH) |
| CTP_INT | GPIO13 | Low when touched |
| I2C addr | 0x38 | 7-bit address |
| I2C baud | 100000 | 100kHz standard mode |

### SD card (reserved)

| Function | GPIO | Notes |
|----------|------|-------|
| SD_CS | GPIO14 | Reserved for TF card, not implemented |

### Boot-sensitive pins

GPIO1, GPIO3, GPIO4, GPIO6, GPIO9, GPIO11 are boot-sensitive pins on the
WS63 (BearPi-Pico H3863). **Do not connect any externally pulled-up signal
to these pins before boot.** The chip will fail to start if these pins are
pulled high during power-on.

**Known issue:** GPIO12 cannot drive HIGH on some boards. If CTP_RST needs
an external drive, tie it directly to 3.3V (VCC).

## Software Configuration

In `src/screen_config.h`:

```c
#define SCREEN_BOARD_REV_FINAL_HW_I2C  1   /* Final board with hardware I2C1 */
#define SCREEN_LCD_ONLY_COLOR_TEST     0   /* 0 = normal mode, 1 = LCD-only test */
```

## Build

```bash
# Ensure CONFIG_ENABLE_SCREEN_SAMPLE=y in .config
python3 build.py -c ws63-liteos-app -ninja -j24
```

Firmware output: `src/output/ws63/fwstage/latest/ws63-liteos-app_screen_all.fwpkg`

## Source files

- `src/screen_config.h`: board pin and bus configuration.
- `src/screen_board.*`: WS63 hardware abstraction for SPI/I2C/GPIO/delay.
- `src/st7796_lcd.*`: ST7796 LCD protocol and drawing primitives.
- `src/st7796_text.*`: 8x16 ASCII text rendering.
- `src/font_ascii.h`: 8x16 monospace ASCII font data (chars 32-126).
- `src/ft6336_touch.*`: FT6336 touch controller driver.

## Current test status

- LCD: ST7796 color test passed (red/green/blue/white/black).
- Touch: FT6336 I2C1 communication verified at 100kHz.
- Display orientation: 320x480 portrait (ST7796_ROTATION_0).
- Touch-to-LCD mapping: initial direct mapping (1:1).
- SD_CS = GPIO14 (reserved, not used). TF/FATFS not implemented.
- Current phase: no LVGL, no SLE, no laser job status integration.
