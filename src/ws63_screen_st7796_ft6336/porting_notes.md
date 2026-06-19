# Porting Notes

## Vendor reference

Source package:

- `4.0inch_SPI/2-Specification/ST7796_Init.txt`
- `4.0inch_SPI/1-Demo/Demo_STM32/.../HARDWARE/LCD/lcd.c`
- `4.0inch_SPI/1-Demo/Demo_STM32/.../HARDWARE/TOUCH/ft6336.c`
- `4.0inch_SPI/4-Driver_IC_Data_Sheet/ST7796S-Sitronix.pdf`
- `4.0inch_SPI/4-Driver_IC_Data_Sheet/DFT6336UDataSheetV1.1.pdf`

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

- LCD: ST7796 color test passed (red/green/blue/white/black).
- Touch: FT6336 I2C1 communication verified at 100kHz.
- Display orientation: 320x480 portrait (ST7796_ROTATION_0).
- Touch-to-LCD mapping: initial direct mapping (1:1). Needs hardware
  verification to determine if swap_xy / invert_x / invert_y is needed.
- SD_CS = GPIO14 (reserved, not used). TF/FATFS not implemented.
- Current phase: no LVGL, no SLE, no laser job status integration.

## Memory plan

Avoid a full framebuffer:

- Full framebuffer: 320 * 480 * 2 = 307200 bytes.
- One line buffer: 320 * 2 = 640 bytes.
- Ten-line tile buffer: 320 * 10 * 2 = 6400 bytes.

The driver in this directory supports direct solid fills and external
RGB565 pixel buffers, so it can later be connected to a partial-refresh
GUI library.
