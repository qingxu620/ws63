# WS63 Screen Port (Deprecated)

**本模块已废弃，活跃开发请见 `src/ws63_screen_panel_lvgl/`。**

This directory is an isolated bring-up area for the WS63 screen node. The
directory name is historical: the project screen selection has changed from the
old 4.0-inch ST7796 module to the MSP3223 reference package.

Current selected module: MSP3223 3.2-inch SPI touch display (ILI9341V LCD + FT6336U touch).

**硬件引脚定义和规格请参见 AGENTS.md Screen Module Rules。**

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
