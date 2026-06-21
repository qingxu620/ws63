# WS63 MSP3223 LVGL Port

WS63 MCU (LiteOS) LVGL v9.3.0 screen node firmware for the MSP3223 module. Provides a laser engraver control panel UI with touch input and SLE wireless RX integration (planned).

## Hardware

| Component | Model | Notes |
|-----------|-------|-------|
| MCU | HiSilicon WS63 (RISC-V, LiteOS-M) | |
| Screen module | MSP3223 3.2" IPS | 18-pin FPC |
| LCD controller | ILI9341V | 240x320 native, RGB565 |
| Touch controller | FT6336U | I2C address 0x38, capacitive |
| Active orientation | Landscape (hardware rotation) | 270° MADCTL, LVGL 320x240 |

## Pin Mapping

### LCD (SPI)

| Function | GPIO | SPI Function |
|----------|------|-------------|
| LCD_SCK  | 7    | SPI0_SCK (mode 3) |
| LCD_MOSI | 9    | SPI0_MOSI (mode 3) |
| LCD_MISO | 11   | SPI0_MISO (mode 3) |
| LCD_CS   | 8    | GPIO output |
| LCD_DC   | 10   | GPIO output |
| LCD_RST  | 0    | GPIO output |
| LCD_BL   | 5    | GPIO output (active high) |

### Touch (I2C)

| Function | GPIO | I2C Function |
|----------|------|-------------|
| CTP_SCL  | 16   | I2C1_SCL (mode 2) |
| CTP_SDA  | 15   | I2C1_SDA (mode 2) |
| CTP_RST  | 12   | GPIO output (active low reset) |
| CTP_INT  | 13   | GPIO input |

### Reserved

| Function | GPIO | Notes |
|----------|------|-------|
| SD_CS    | 14   | Reserved, held high |

## Build

```bash
cd /root/fbb_ws63
./scripts/build_screen_firmware.sh --lvgl
```

Output: `src/output/ws63/fwstage/latest/ws63-liteos-app_screen_all.fwpkg`

Flash with BurnTool on Windows (manual step).

## File Structure

```
src/ws63_screen_lvgl/
├── main.c                  Entry point, task creation, init sequence
├── lv_conf.h               LVGL v9.3.0 configuration
├── CMakeLists.txt          Build configuration
├── Kconfig                 Menuconfig options
├── README.md               This file
└── src/
    ├── screen_config.h     Board pins, resolution, I2C/SPI config
    ├── ili9341_lcd.c/h     ILI9341V SPI LCD driver
    ├── lv_port_disp.c      LVGL display port (flush callback)
    ├── lv_port_indev.c     LVGL input device port (touch callback)
    ├── lv_demo_panel.c/h   Demo UI: WS63 Laser Panel
    └── lvgl/               LVGL v9.3.0 source (submodule)
```

External dependencies (from deprecated project, referenced via CMakeLists.txt):

```
src/deprecated/ws63_screen_st7796_ft6336/src/
├── screen_board.c/h        Hardware abstraction (GPIO, SPI, I2C)
├── ft6336_touch.c/h        FT6336U touch driver
```

## Display Configuration

- **Pixel format**: RGB565, little-endian byte order (`LV_COLOR_16_SWAP=1`)
- **SPI clock**: 32 MHz
- **Display buffer**: 320x48 partial-render single buffer (30,720 bytes)
- **Reset sequence**: LOW 100ms, HIGH 50ms
- **MADCTL**: 0x68 (BGR + MV + MX) for 270° landscape rotation

## UI Layout (Demo Panel)

```
┌─────────────────────────────────┐
│  WS63 Laser Panel               │
│  RX: DISCONNECTED  (orange-red) │
│  No Job                         │
│  ████████████████  0%           │
│  [Start] [Pause] [Stop]         │
│  [■红] [■绿] [■蓝]  RGB check   │
│  Touch: x=--- y=---             │
└─────────────────────────────────┘
```

- Start/Pause/Stop buttons are interactive (fake state transitions)
- RGB color bars verify R/G/B channel correctness
- Touch coordinates update in real-time when screen is touched

## Touch Coordinate Mapping

FT6336U reports portrait-native coordinates. The port maps them to landscape LVGL space:

```c
lv_x = 319 - raw_y;
lv_y = raw_x;
```

Origin (0,0) is top-left of the landscape view. X increases right, Y increases down.

## Current Status

- [x] ILI9341 LCD driver (init, window, pixel write)
- [x] Correct orientation (270° landscape)
- [x] Correct RGB565 color rendering
- [x] FT6336U touch driver (I2C, coordinate read)
- [x] Touch coordinate mapping to LVGL
- [x] LVGL demo panel with buttons and status labels
- [x] RGB color channel verification bars
- [ ] SLE wireless RX data integration
- [ ] Real laser job status display
- [ ] Touch gesture / long-press handling

## Build Variants

This module uses `CONFIG_ENABLE_LVGL_SAMPLE=y`. It is mutually exclusive with `CONFIG_ENABLE_SCREEN_SAMPLE` (deprecated self-test page).

Build script automatically disables competing samples (`LASER_SLE_JOB_SAMPLE`, `LASER_RX_UNIFIED`, `SCREEN_SAMPLE`) before building.
