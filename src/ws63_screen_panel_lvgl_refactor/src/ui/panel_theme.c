/**
 * @file panel_theme.c
 * @brief Industrial panel theme implementation.
 */
#include "panel_theme.h"

lv_style_t style_screen;
lv_style_t style_card;
lv_style_t style_text_bright;
lv_style_t style_text_light;
lv_style_t style_text_muted;

void panel_theme_init(void)
{
    lv_style_init(&style_screen);
    lv_style_set_bg_color(&style_screen, COLOR_BG_DARK);
    lv_style_set_bg_opa(&style_screen, LV_OPA_COVER);
    lv_style_set_border_width(&style_screen, 0);
    lv_style_set_radius(&style_screen, 0);
    lv_style_set_pad_all(&style_screen, 0);

    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, COLOR_BG_CARD);
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_border_color(&style_card, COLOR_BORDER);
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_border_opa(&style_card, LV_OPA_COVER);
    lv_style_set_radius(&style_card, 6);
    lv_style_set_pad_all(&style_card, 6);

    lv_style_init(&style_text_bright);
    lv_style_set_text_color(&style_text_bright, COLOR_TEXT_BRIGHT);

    lv_style_init(&style_text_light);
    lv_style_set_text_color(&style_text_light, COLOR_TEXT_LIGHT);

    lv_style_init(&style_text_muted);
    lv_style_set_text_color(&style_text_muted, COLOR_TEXT_MUTED);
}

void panel_page_body_setup(lv_obj_t *body, int32_t row_gap)
{
    lv_obj_set_size(body, 304, 200);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 8, 40);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE |
                          LV_OBJ_FLAG_SCROLL_ELASTIC |
                          LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_style_pad_hor(body, 0, 0);
    lv_obj_set_style_pad_ver(body, 4, 0);
    lv_obj_set_style_pad_gap(body, row_gap, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);

    lv_obj_set_style_width(body, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(body, COLOR_LASER_BLUE, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(body, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(body, 2, LV_PART_SCROLLBAR);
}
