/**
 * @file panel_theme.c
 * @brief Industrial panel theme: color tokens and reusable styles.
 */
#include "panel_theme.h"

lv_style_t style_screen;
lv_style_t style_card;
lv_style_t style_text_bright;
lv_style_t style_text_light;
lv_style_t style_text_muted;
lv_style_t style_btn_default;
lv_style_t style_btn_green;
lv_style_t style_btn_yellow;
lv_style_t style_btn_red;
lv_style_t style_btn_orange;
lv_style_t style_btn_disabled;

void panel_theme_init(void)
{
    /* Screen background */
    lv_style_init(&style_screen);
    lv_style_set_bg_color(&style_screen, COLOR_BG_DARK);
    lv_style_set_bg_opa(&style_screen, LV_OPA_COVER);
    lv_style_set_border_width(&style_screen, 0);
    lv_style_set_radius(&style_screen, 0);
    lv_style_set_pad_all(&style_screen, 0);

    /* Card container */
    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, COLOR_BG_CARD);
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_border_color(&style_card, COLOR_BORDER);
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_border_opa(&style_card, LV_OPA_COVER);
    lv_style_set_radius(&style_card, 6);
    lv_style_set_pad_all(&style_card, 6);

    /* Text styles */
    lv_style_init(&style_text_bright);
    lv_style_set_text_color(&style_text_bright, COLOR_TEXT_BRIGHT);

    lv_style_init(&style_text_light);
    lv_style_set_text_color(&style_text_light, COLOR_TEXT_LIGHT);

    lv_style_init(&style_text_muted);
    lv_style_set_text_color(&style_text_muted, COLOR_TEXT_MUTED);

    /* Button base: solid capsule, no border, radius 12 */
    lv_style_init(&style_btn_default);
    lv_style_set_radius(&style_btn_default, 12);
    lv_style_set_border_width(&style_btn_default, 0);
    lv_style_set_bg_opa(&style_btn_default, LV_OPA_COVER);
    lv_style_set_text_color(&style_btn_default, lv_color_white());
    lv_style_set_pad_all(&style_btn_default, 0);

    /* Start: green */
    lv_style_init(&style_btn_green);
    lv_style_set_bg_color(&style_btn_green, lv_color_hex(0x10B981));

    /* Pause: yellow */
    lv_style_init(&style_btn_yellow);
    lv_style_set_bg_color(&style_btn_yellow, lv_color_hex(0xFFB300));

    /* Stop: red */
    lv_style_init(&style_btn_red);
    lv_style_set_bg_color(&style_btn_red, lv_color_hex(0xFF3366));

    /* Reset: orange */
    lv_style_init(&style_btn_orange);
    lv_style_set_bg_color(&style_btn_orange, lv_color_hex(0xFF9900));

    /* Disabled: dark muted */
    lv_style_init(&style_btn_disabled);
    lv_style_set_bg_color(&style_btn_disabled, lv_color_hex(0x232B38));
    lv_style_set_bg_opa(&style_btn_disabled, LV_OPA_COVER);
    lv_style_set_text_color(&style_btn_disabled, lv_color_hex(0x5A6578));
}
