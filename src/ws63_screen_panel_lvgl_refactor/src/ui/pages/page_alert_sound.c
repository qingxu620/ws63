/**
 * @file page_alert_sound.c
 * @brief Alert Sound page: tone toggles (buttons) + volume arcs (no real audio).
 */
#include "page_alert_sound.h"
#include "panel_theme.h"
#include "ui_manager.h"
#include "soc_osal.h"
#include <stdio.h>

typedef struct {
    bool enabled;
    int volume;
    lv_obj_t *btn_toggle;
    lv_obj_t *lbl_toggle;
    lv_obj_t *arc_vol;
    lv_obj_t *lbl_vol;
    lv_color_t accent;
    const char *name;
} tone_entry_t;

static tone_entry_t g_tones[3];

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_switch_page(PAGE_SETTINGS);
}

static void toggle_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    tone_entry_t *entry = (tone_entry_t *)lv_event_get_user_data(e);
    entry->enabled = !entry->enabled;

    if (entry->enabled) {
        lv_obj_set_style_bg_color(btn, entry->accent, 0);
        lv_label_set_text(entry->lbl_toggle, LV_SYMBOL_OK " 开");
    } else {
        lv_obj_set_style_bg_color(btn, COLOR_BG_CARD, 0);
        lv_label_set_text(entry->lbl_toggle, LV_SYMBOL_CLOSE " 关");
    }
    osal_printk("[SOUND] %s %s\r\n", entry->name, entry->enabled ? "ON" : "OFF");
}

static void vol_cb(lv_event_t *e)
{
    lv_obj_t *arc = lv_event_get_target(e);
    tone_entry_t *entry = (tone_entry_t *)lv_event_get_user_data(e);
    entry->volume = lv_arc_get_value(arc);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", entry->volume);
    lv_label_set_text(entry->lbl_vol, buf);
}

static void create_tone_row(lv_obj_t *parent, int idx, const char *name,
                            const char *symbol, lv_color_t accent)
{
    tone_entry_t *t = &g_tones[idx];
    t->enabled = true;
    t->volume = 80;
    t->accent = accent;
    t->name = name;

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 290, 56);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_style(card, &style_card, 0);

    /* Left: name + toggle */
    lv_obj_t *left = lv_obj_create(card);
    lv_obj_set_size(left, 130, 40);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(left, 2, 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);

    lv_obj_t *name_lbl = lv_label_create(left);
    lv_label_set_text_fmt(name_lbl, "%s %s", symbol, name);
    lv_obj_set_style_text_font(name_lbl, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(name_lbl, accent, 0);

    t->btn_toggle = lv_button_create(left);
    lv_obj_set_size(t->btn_toggle, 80, 22);
    lv_obj_set_style_bg_color(t->btn_toggle, accent, 0);
    lv_obj_set_style_radius(t->btn_toggle, 6, 0);

    t->lbl_toggle = lv_label_create(t->btn_toggle);
    lv_label_set_text(t->lbl_toggle, LV_SYMBOL_OK " 开");
    lv_obj_set_style_text_font(t->lbl_toggle, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(t->lbl_toggle, lv_color_white(), 0);
    lv_obj_center(t->lbl_toggle);
    lv_obj_add_event_cb(t->btn_toggle, toggle_cb, LV_EVENT_CLICKED, t);

    /* Right: volume arc + label */
    lv_obj_t *right = lv_obj_create(card);
    lv_obj_set_size(right, 120, 40);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(right, 4, 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);

    t->arc_vol = lv_arc_create(right);
    lv_obj_set_size(t->arc_vol, 36, 36);
    lv_arc_set_bg_angles(t->arc_vol, 0, 270);
    lv_arc_set_angles(t->arc_vol, 0, 216);
    lv_arc_set_range(t->arc_vol, 0, 100);
    lv_arc_set_value(t->arc_vol, 80);
    lv_obj_set_style_arc_width(t->arc_vol, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(t->arc_vol, COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(t->arc_vol, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(t->arc_vol, accent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(t->arc_vol, COLOR_TEXT_BRIGHT, LV_PART_KNOB);
    lv_obj_add_event_cb(t->arc_vol, vol_cb, LV_EVENT_VALUE_CHANGED, t);

    t->lbl_vol = lv_label_create(right);
    lv_label_set_text(t->lbl_vol, "80%");
    lv_obj_set_style_text_font(t->lbl_vol, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(t->lbl_vol, COLOR_TEXT_LIGHT, 0);
}

void page_alert_sound_create(lv_obj_t *parent)
{
    lv_obj_t *scr = parent;
    lv_obj_remove_style_all(scr);
    lv_obj_add_style(scr, &style_screen, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Header */
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, 320, 32);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, 8, 0);
    lv_obj_set_style_pad_gap(header, 8, 0);
    lv_obj_set_style_bg_color(header, COLOR_BG_CARD, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(header, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(header, 0, 0);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 40, 28);
    lv_obj_set_style_bg_color(back, COLOR_BG_CARD, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_border_color(back, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_lbl, COLOR_TEXT_BRIGHT, 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "提示音设置");
    lv_obj_set_style_text_font(title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(title, COLOR_TEXT_BRIGHT, 0);

    /* Body */
    lv_obj_t *body = lv_obj_create(scr);
    panel_page_body_setup(body, 8);

    create_tone_row(body, 0, "任务完成", LV_SYMBOL_OK, COLOR_LASER_GREEN);
    create_tone_row(body, 1, "错误告警", LV_SYMBOL_WARNING, COLOR_LASER_RED);
    create_tone_row(body, 2, "触摸反馈", LV_SYMBOL_BELL, COLOR_LASER_BLUE);
}

void page_alert_sound_update(void)
{
}
