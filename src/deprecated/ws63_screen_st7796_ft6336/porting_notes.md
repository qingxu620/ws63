# Porting Notes

## Vendor reference

Current selected source package:

- `MSP3223/init/ILI9341V_Init.txt`
- `MSP3223/docs/driver_ic/ILI9341_Datasheet.pdf`
- `MSP3223/docs/driver_ic/DFT6336UDataSheetV1.1.pdf`
- `MSP3223/docs/driver_ic/FT6336U_Register.xlsx`
- `MSP3223/docs/specification/MSP3222_MSP3223_Specification_CN_V1.0.pdf`
- `MSP3223/docs/schematic/3.2inch_SPI_Module_MSP3222_MSP3223_Schematic.pdf`

The old `src/4.0inch_SPI/` ST7796 reference package is no longer the active
screen selection and has been removed from the project tree. Existing source
filenames in this directory still use the historical ST7796 naming until the
MSP3223 ILI9341V port is completed.

Current selected hardware:

- Module: MSP3223 3.2-inch SPI touch display
- LCD controller: ILI9341V
- Resolution: 240x320 RGB565 (`LCD_W=240`, `LCD_H=320` in the vendor init)
- Touch controller: FT6336U at 0x38

## LCD signals (final board)

| Module pin | Function | WS63 side |
| --- | --- | --- |
| VCC | Power | 3.3V |
| GND | Ground | GND |
| SDI / MOSI | LCD SPI data in | GPIO9 (SPI0_MOSI) |
| SDO / MISO | LCD SPI data out | GPIO11 (SPI0_MISO, optional) |
| SCK | LCD SPI clock | GPIO7 (SPI0_SCK) |
| LCD_RS | Command/data select | GPIO10 |
| LCD_RST | LCD reset | GPIO0 (active low) |
| LCD_CS | LCD chip select | GPIO8 (active low) |
| LED | Backlight | GPIO5 (high=on) |
| SD_CS | TF card chip select | GPIO14 (reserved, not used) |

## Touch signals (final board, hardware I2C1)

| Module pin | Function | WS63 side |
| --- | --- | --- |
| CTP_SCL | FT6336 I2C clock | GPIO16 / I2C1_SCL (hardware I2C1, 100kHz) |
| CTP_SDA | FT6336 I2C data | GPIO15 / I2C1_SDA (hardware I2C1, 100kHz) |
| CTP_RST | Touch reset | GPIO12 (tie to 3.3V if GPIO12 can't drive HIGH) |
| CTP_INT | Touch interrupt | GPIO13 (low when touched) |

Pinmux: GPIO16 and GPIO15 set to PIN_MODE_2 (I2C1 alternate function).
SDA/SCL lines need 4.7kΩ pull-up to 3.3V.

## Electrical notes

- Keep all logic at 3.3V unless the module documentation clearly states
  the board contains level shifting.
- Do not drive LCD SPI at the maximum rate first. Start at 8 to 12 MHz,
  then raise it after color-fill tests are stable.
- If the backlight pin draws too much current, drive it through a MOSFET
  instead of directly from a WS63 GPIO.

## Boot-sensitive pins

GPIO1, GPIO3, GPIO4, GPIO6, GPIO9, GPIO11 are boot-sensitive pins on the
WS63 (BearPi-Pico H3863). **Do not connect any externally pulled-up signal
to these pins before boot.** The chip will fail to start if these pins are
pulled high during power-on.

**Known issue:** GPIO12 cannot drive HIGH on some boards. If CTP_RST needs
an external drive, tie it directly to 3.3V (VCC).

## SD card (reserved)

SD_CS is assigned to GPIO14 (active low). This pin is reserved for future
TF card support. TF/FATFS is not implemented in this phase.

## Current test status

- MSP3223/ILI9341V LCD port: pending.
- MSP3223/FT6336U touch verification: pending.
- Old ST7796 LCD color test: historical pass only, not the current selected module.
- Old FT6336 I2C1 communication: historical pass only; FT6336U remains the touch target for MSP3223.
- Display orientation: pending MSP3223 ILI9341V bring-up.
- Touch-to-LCD mapping: pending MSP3223 hardware verification to determine if
  swap_xy / invert_x / invert_y is needed.
- SD_CS = GPIO14 (reserved, not used). TF/FATFS not implemented.
- Current phase: no LVGL, no SLE, no laser job status integration.

## SPI / I2C speed tuning

### LCD SPI speed

Default: 16 MHz. Configurable via `SCREEN_LCD_SPI_BAUDRATE` in screen_config.h.

Recommended test sequence:
1. 8 MHz — baseline, should always work
2. 12 MHz — moderate speed
3. 16 MHz — default, good balance
4. 20 MHz — high speed
5. 24 MHz — maximum, may not be stable on all boards

Test criteria:
- Fill screen with solid color: no noise, no flicker
- Fill screen with alternating pattern: no color shift
- Touch polling during LCD write: no lost data
- Run for >10 minutes continuously

### Touch I2C1 speed

Default: 100 kHz. Configurable via `SCREEN_TOUCH_I2C_BAUDRATE` in screen_config.h.

- 100 kHz: standard mode, always stable
- 400 kHz: fast mode, test after 100kHz is confirmed stable

### Speed test results

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
verification. I2C1 1MHz is not pursued unless the 100kHz/400kHz path proves
insufficient.

Test 1: LCD color test OK, self-test page OK, FT6336 probe 0x38 OK, touch init OK.

## Memory plan

Avoid a full framebuffer:

- Full MSP3223 framebuffer: 240 * 320 * 2 = 153600 bytes.
- One 240-pixel line buffer: 240 * 2 = 480 bytes.
- Ten-line 240-pixel tile buffer: 240 * 10 * 2 = 4800 bytes.

The driver in this directory supports direct solid fills and external
RGB565 pixel buffers, so it can later be connected to a partial-refresh
GUI library.
