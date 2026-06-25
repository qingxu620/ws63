/**
 * @file page_control.c
 * @brief Control Panel page: focus arc + manual jog buttons.
 */
#include "page_control.h"
#include "panel_theme.h"
#include "ui_manager.h"
#include "../service/panel_model.h"
#include "soc_osal.h"
#include <stdio.h>

static lv_obj_t *g_arc_focus;
static lv_obj_t *g_lbl_focus_val;
static lv_obj_t *g_lbl_focus_state;

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_switch_page(PAGE_HOME);
}

static void focus_arc_cb(lv_event_t *e)
{
    lv_obj_t *arc = lv_event_get_target(e);
    int val = lv_arc_get_value(arc);
    char buf[8];
    snprintf(buf, sizeof(buf), "S%d", val);
    lv_label_set_text(g_lbl_focus_val, buf);
}

static void focus_on_cb(lv_event_t *e)
{
    (void)e;
    lv_label_set_text(g_lbl_focus_state, "激光开启");
    lv_obj_set_style_text_color(g_lbl_focus_state, COLOR_LASER_RED, 0);
}

static void focus_off_cb(lv_event_t *e)
{
    (void)e;
    lv_label_set_text(g_lbl_focus_state, "激光关闭");
    lv_obj_set_style_text_color(g_lbl_focus_state, COLOR_LASER_GREEN, 0);
}

static void jog_cb(lv_event_t *e)
{
    const char *dir = (const char *)lv_event_get_user_data(e);
    (void)dir;
}

static lv_obj_t *create_jog_btn(lv_obj_t *parent, const char *symbol, const char *dir)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 48, 36);
    lv_obj_set_style_bg_color(btn, COLOR_BG_CARD, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_color(btn, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_color(lbl, COLOR_TEXT_BRIGHT, 0);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, jog_cb, LV_EVENT_CLICKED, (void *)dir);
    return btn;
}

void page_control_create(lv_obj_t *parent)
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
    lv_label_set_text(title, "手动控制");
    lv_obj_set_style_text_font(title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(title, COLOR_TEXT_BRIGHT, 0);

    /* Body */
    lv_obj_t *body = lv_obj_create(scr);
    panel_page_body_setup(body, 8);

    /* Focus control card */
    lv_obj_t *focus_card = lv_obj_create(body);
    lv_obj_set_size(focus_card, 290, 80);
    lv_obj_remove_flag(focus_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(focus_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(focus_card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_style(focus_card, &style_card, 0);

    /* Left: info + buttons */
    lv_obj_t *left = lv_obj_create(focus_card);
    lv_obj_set_size(left, 170, 64);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(left, 4, 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);

    /* Focus header */
    lv_obj_t *fh = lv_obj_create(left);
    lv_obj_set_size(fh, 160, 16);
    lv_obj_remove_flag(fh, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(fh, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fh, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(fh, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(fh, 0, 0);
    lv_obj_set_style_pad_all(fh, 0, 0);

    lv_obj_t *ft = lv_label_create(fh);
    lv_label_set_text(ft, LV_SYMBOL_EYE_OPEN " 调焦功率");
    lv_obj_set_style_text_font(ft, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(ft, COLOR_LASER_ORANGE, 0);

    g_lbl_focus_val = lv_label_create(fh);
    lv_label_set_text(g_lbl_focus_val, "S10");
    lv_obj_set_style_text_font(g_lbl_focus_val, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(g_lbl_focus_val, COLOR_TEXT_BRIGHT, 0);

    g_lbl_focus_state = lv_label_create(fh);
    lv_label_set_text(g_lbl_focus_state, "激光关闭");
    lv_obj_set_style_text_font(g_lbl_focus_state, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(g_lbl_focus_state, COLOR_LASER_GREEN, 0);

    /* ON/OFF buttons */
    lv_obj_t *btns = lv_obj_create(left);
    lv_obj_set_size(btns, 160, 28);
    lv_obj_remove_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btns, 0, 0);
    lv_obj_set_style_pad_all(btns, 0, 0);

    lv_obj_t *btn_on = lv_button_create(btns);
    lv_obj_set_size(btn_on, 72, 24);
    lv_obj_set_style_bg_color(btn_on, COLOR_LASER_RED, 0);
    lv_obj_set_style_radius(btn_on, 6, 0);
    lv_obj_t *lbl_on = lv_label_create(btn_on);
    lv_label_set_text(lbl_on, "开启");
    lv_obj_set_style_text_font(lbl_on, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(lbl_on, lv_color_white(), 0);
    lv_obj_center(lbl_on);
    lv_obj_add_event_cb(btn_on, focus_on_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_off = lv_button_create(btns);
    lv_obj_set_size(btn_off, 72, 24);
    lv_obj_set_style_bg_color(btn_off, COLOR_LASER_GREEN, 0);
    lv_obj_set_style_radius(btn_off, 6, 0);
    lv_obj_t *lbl_off = lv_label_create(btn_off);
    lv_label_set_text(lbl_off, "关闭");
    lv_obj_set_style_text_font(lbl_off, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(lbl_off, lv_color_white(), 0);
    lv_obj_center(lbl_off);
    lv_obj_add_event_cb(btn_off, focus_off_cb, LV_EVENT_CLICKED, NULL);

    /* Right: focus power arc */
    g_arc_focus = lv_arc_create(focus_card);
    lv_obj_set_size(g_arc_focus, 60, 60);
    lv_arc_set_bg_angles(g_arc_focus, 0, 360);
    lv_arc_set_angles(g_arc_focus, 0, 36);
    lv_arc_set_range(g_arc_focus, 0, 100);
    lv_arc_set_value(g_arc_focus, 10);
    lv_obj_set_style_arc_width(g_arc_focus, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_arc_focus, COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_arc_focus, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_arc_focus, COLOR_LASER_RED, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_arc_focus, COLOR_TEXT_BRIGHT, LV_PART_KNOB);
    lv_obj_add_event_cb(g_arc_focus, focus_arc_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Jog control card */
    lv_obj_t *jog_card = lv_obj_create(body);
    lv_obj_set_size(jog_card, 290, 104);
    lv_obj_remove_flag(jog_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(jog_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(jog_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(jog_card, 4, 0);
    lv_obj_add_style(jog_card, &style_card, 0);

    lv_obj_t *jog_title = lv_label_create(jog_card);
    lv_label_set_text(jog_title, LV_SYMBOL_LOOP " 手动点动");
    lv_obj_set_style_text_font(jog_title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(jog_title, COLOR_LASER_BLUE, 0);

    /* Top row: spacer + UP + spacer */
    lv_obj_t *row_top = lv_obj_create(jog_card);
    lv_obj_set_size(row_top, 180, 32);
    lv_obj_remove_flag(row_top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row_top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_top, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row_top, 4, 0);
    lv_obj_set_style_bg_opa(row_top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row_top, 0, 0);
    lv_obj_set_style_pad_all(row_top, 0, 0);

    lv_obj_t *sp1 = lv_obj_create(row_top);
    lv_obj_set_size(sp1, 48, 1);
    lv_obj_set_style_bg_opa(sp1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp1, 0, 0);
    create_jog_btn(row_top, LV_SYMBOL_UP, "Y+");
    lv_obj_t *sp2 = lv_obj_create(row_top);
    lv_obj_set_size(sp2, 48, 1);
    lv_obj_set_style_bg_opa(sp2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp2, 0, 0);

    /* Middle row: LEFT + HOME + RIGHT */
    lv_obj_t *row_mid = lv_obj_create(jog_card);
    lv_obj_set_size(row_mid, 180, 32);
    lv_obj_remove_flag(row_mid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row_mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row_mid, 4, 0);
    lv_obj_set_style_bg_opa(row_mid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row_mid, 0, 0);
    lv_obj_set_style_pad_all(row_mid, 0, 0);

    create_jog_btn(row_mid, LV_SYMBOL_LEFT, "X-");
    create_jog_btn(row_mid, LV_SYMBOL_HOME, "HOME");
    create_jog_btn(row_mid, LV_SYMBOL_RIGHT, "X+");
}

void page_control_update(void)
{
}
