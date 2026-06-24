/**
 * @file panel_theme.h
 * @brief Industrial panel theme: color tokens and styles.
 */
#ifndef PANEL_THEME_H
#define PANEL_THEME_H

#include "lvgl.h"

LV_FONT_DECLARE(lv_font_panel_cn_14);
#define PANEL_FONT_CN (&lv_font_panel_cn_14)

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

void panel_theme_init(void);
void panel_page_body_setup(lv_obj_t *body, int32_t row_gap);

extern lv_style_t style_screen;
extern lv_style_t style_card;
extern lv_style_t style_text_bright;
extern lv_style_t style_text_light;
extern lv_style_t style_text_muted;

#endif
