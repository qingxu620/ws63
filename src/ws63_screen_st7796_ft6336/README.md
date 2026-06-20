# WS63 Screen Port

This directory is an isolated bring-up area for the WS63 screen node. The
directory name is historical: the project screen selection has changed from the
old 4.0-inch ST7796 module to the MSP3223 reference package.

Current selected module:

- Vendor/reference package: `MSP3223/`
- Module: MSP3223 3.2-inch SPI touch display
- LCD controller: ILI9341V
- LCD resolution: 240x320 RGB565
- Touch controller: FT6336U, I2C address 0x38
- LCD init reference: `MSP3223/init/ILI9341V_Init.txt`
- LCD datasheet: `MSP3223/docs/driver_ic/ILI9341_Datasheet.pdf`
- Touch datasheet/registers: `MSP3223/docs/driver_ic/DFT6336UDataSheetV1.1.pdf`,
  `MSP3223/docs/driver_ic/FT6336U_Register.xlsx`

The source in this directory still contains the earlier ST7796 bring-up code.
That code must be ported to ILI9341V before it is considered current for the
MSP3223 hardware.

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
#define SCREEN_LCD_SPI_BAUDRATE  16000000  /* SPI clock: 8/12/16/20/24 MHz */
#define SCREEN_TOUCH_I2C_BAUDRATE 100000   /* I2C1: 100kHz or 400kHz */
```

### SPI / I2C Speed Test Recommendations

| Interface | Test Range | Notes |
|-----------|-----------|-------|
| LCD SPI | 8 / 12 / 16 / 20 / 24 MHz | Start low, increase until stable |
| Touch I2C1 | 100kHz / 400kHz | Default 100kHz, 400kHz after validation |

**Stability criteria:**
- No screen glitches or color errors
- Touch data not lost during LCD updates
- Long-term stable operation (>10 minutes)

**Flywire environment:** Do not default to high SPI frequencies. Start at 8MHz
and increase step by step. Breadboard wires have significant parasitic
capacitance that limits signal integrity at high frequencies.

### Speed Test Results

| Config | SPI | I2C1 | Result | Date |
|--------|-----|------|--------|------|
| 1 | 16 MHz | 100 kHz | **PASS** | 2026-06-20 |
| 2 | 20 MHz | 100 kHz | Skipped | — |
| 3 | 24 MHz | 100 kHz | **PASS** | 2026-06-20 |
| 4 | 32 MHz | 100 kHz | **PASS** | 2026-06-20 |
| 5 | 32 MHz | 400 kHz | **PASS** | 2026-06-20 |

**Historical validated baseline for the old ST7796 module:**
- ST7796 LCD over SPI0 @ 32MHz
- FT6336U touch over I2C1 @ 400kHz

**Current MSP3223 target baseline:** pending ILI9341V driver port and hardware
verification.

I2C1 1MHz is not pursued; current touch responsiveness already meets requirements.

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

- MSP3223/ILI9341V port: pending.
- Old ST7796 LCD color test: historical pass only, not the current selected module.
- Old FT6336 I2C1 communication: historical pass only; FT6336U remains the touch target for MSP3223.
- Target display orientation for MSP3223: 240x320 portrait or 320x240 landscape, to be confirmed during ILI9341V bring-up.
- Touch-to-LCD mapping: pending MSP3223 hardware verification.
- SD_CS = GPIO14 (reserved, not used). TF/FATFS not implemented.
- Current phase: no LVGL, no SLE, no laser job status integration.
