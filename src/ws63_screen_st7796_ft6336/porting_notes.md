# Porting Notes

## Vendor reference

Source package:

- `4.0inch_SPI/2-Specification/ST7796_Init.txt`
- `4.0inch_SPI/1-Demo/Demo_STM32/.../HARDWARE/LCD/lcd.c`
- `4.0inch_SPI/1-Demo/Demo_STM32/.../HARDWARE/TOUCH/ft6336.c`
- `4.0inch_SPI/4-Driver_IC_Data_Sheet/ST7796S-Sitronix.pdf`
- `4.0inch_SPI/4-Driver_IC_Data_Sheet/DFT6336UDataSheetV1.1.pdf`

## LCD signals

| Module pin | Function | WS63 side |
| --- | --- | --- |
| VCC | Power | 3.3 V preferred, confirm module jumper/regulator |
| GND | Ground | GND |
| SDI / MOSI | LCD SPI data in | SPI MOSI |
| SDO / MISO | LCD SPI data out | Optional SPI MISO |
| SCK | LCD SPI clock | SPI SCK |
| LCD_RS | Command/data select | GPIO output |
| LCD_RST | LCD reset | GPIO output |
| LCD_CS | LCD chip select | GPIO output |
| LED | Backlight | GPIO output or PWM |

## Touch signals

| Module pin | Function | WS63 side |
| --- | --- | --- |
| CTP_SCL | FT6336 I2C clock | I2C SCL |
| CTP_SDA | FT6336 I2C data | I2C SDA |
| CTP_RST | Touch reset | GPIO output |
| CTP_INT | Touch interrupt | GPIO input, optional initially |

## Electrical notes

- Keep all logic at 3.3 V unless the module documentation clearly states
  the board contains level shifting.
- Do not drive LCD SPI at the maximum rate first. Start at 8 to 12 MHz,
  then raise it after color-fill tests are stable.
- If the backlight pin draws too much current, drive it through a MOSFET
  instead of directly from a WS63 GPIO.

## Memory plan

Avoid a full framebuffer:

- Full framebuffer: 320 * 480 * 2 = 307200 bytes.
- One line buffer: 320 * 2 = 640 bytes.
- Ten-line tile buffer: 320 * 10 * 2 = 6400 bytes.

The driver in this directory supports direct solid fills and external
RGB565 pixel buffers, so it can later be connected to a partial-refresh
GUI library.
