# WS63 ST7796 + FT6336 Screen Port

This directory is an isolated bring-up area for the
`4.0inch_SPI_Module_ST7796_MSP4030_MSP4031_V1.0` touch display module.
It is not wired into the existing laser marker build yet.

## Suitability for WS63

The module is suitable for a dedicated WS63 screen board.

- LCD controller: ST7796S
- LCD resolution: 320 x 480
- LCD bus: 4-wire SPI plus DC, CS, RST, BL GPIOs
- Pixel format: RGB565, 16 bits per pixel
- Touch controller: FT6336U
- Touch bus: I2C, 7-bit address 0x38
- Touch points: up to 2 points

WS63 has enough interfaces for this module:

- SPI master for ST7796 LCD writes
- I2C master for FT6336 touch reads
- GPIO for LCD DC/CS/RST and touch RST/INT
- GPIO or PWM for LCD backlight

The main limitation is RAM and refresh bandwidth. A full 320 x 480 x 2
framebuffer uses 307200 bytes, which is too large for a product firmware
that also runs wireless communication and UI logic. Use line buffers,
small tile buffers, or LVGL partial rendering.

## Recommended bring-up order

1. Wire power, ground, LCD SPI, LCD DC/CS/RST/BL.
2. Build only the LCD path. Run `screen_demo_run()` and verify solid
   red, green, blue, black and white fills.
3. Wire touch I2C, touch RST and INT.
4. Read FT6336 IDs: FocalTech ID 0x11, cipher mid 0x26, cipher high 0x64.
5. Poll touch coordinates and verify rotation mapping.
6. Add a small UI layer or LVGL partial-refresh port.
7. Add SLE command/control communication with the laser transmitter or
   receiver board.

## Source files

- `src/screen_config.h`: board pin and bus configuration.
- `src/screen_board.*`: WS63 hardware abstraction for SPI/I2C/GPIO/delay.
- `src/st7796_lcd.*`: ST7796 LCD protocol and drawing primitives.
- `src/ft6336_touch.*`: FT6336 touch controller driver.
- `src/screen_demo.c`: simple LCD and touch bring-up test.

## Pin warning

The default pins in `screen_config.h` are placeholders. Confirm them
against the actual WS63 board schematic before building firmware. This
screen board should stay separate from `ws63_laser_single` so the stable
single-board laser firmware remains untouched.
