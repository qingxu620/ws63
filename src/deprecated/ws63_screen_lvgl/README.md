# WS63 MSP3223 LVGL Port (Deprecated)

**本模块已废弃，活跃开发请见 `src/ws63_screen_panel_lvgl/`。**

WS63 MCU (LiteOS) LVGL v9.3.0 screen node firmware for the MSP3223 module. Provides a laser engraver control panel UI with touch input and SLE wireless RX integration (planned).

## Hardware

MSP3223 3.2-inch SPI touch display (ILI9341V LCD + FT6336U touch).

**硬件引脚定义和规格请参见 AGENTS.md Screen Module Rules。**

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
