/**
 * @file panel_theme.h
 * @brief Color tokens and global style definitions for the industrial panel UI.
 */
#ifndef PANEL_THEME_H
#define PANEL_THEME_H

#include "lvgl.h"

/* Color tokens */
#define COLOR_BG_DARK       lv_color_hex(0x0A0C10)
#define COLOR_BG_CARD       lv_color_hex(0x121620)
#define COLOR_BORDER        lv_color_hex(0x262F45)
#define COLOR_TEXT_BRIGHT   lv_color_hex(0xFFFFFF)
#define COLOR_TEXT_LIGHT    lv_color_hex(0xE2E8F0)
#define COLOR_TEXT_MUTED    lv_color_hex(0x6B7C96)
#define COLOR_LASER_GREEN   lv_color_hex(0x00FFCC)
#define COLOR_LASER_RED     lv_color_hex(0xFF3366)
#define COLOR_LASER_YELLOW  lv_color_hex(0xFFCC00)
#define COLOR_LASER_ORANGE  lv_color_hex(0xFF9900)
#define COLOR_LASER_BLUE    lv_color_hex(0x00B3FF)
#define COLOR_TRACK_BG      lv_color_hex(0x0C0F16)

void panel_theme_init(void);

/* Pre-built styles (global, read-only after init) */
extern lv_style_t style_screen;
extern lv_style_t style_card;
extern lv_style_t style_text_bright;
extern lv_style_t style_text_light;
extern lv_style_t style_text_muted;
extern lv_style_t style_btn_default;
extern lv_style_t style_btn_green;
extern lv_style_t style_btn_yellow;
extern lv_style_t style_btn_red;
extern lv_style_t style_btn_orange;
extern lv_style_t style_btn_disabled;

#endif
