/**
 * @file panel_ui.c
 * @brief Industrial panel UI for WS63 laser engraver.
 *
 * Layout (320x240 landscape):
 *   status_bar   (0,0)   320x24
 *   body         (0,24)  320x168
 *     progress_block  (8,32)   114x152
 *     info_block       (130,32) 174x152
 *   action_bar   (0,192) 320x48
 */
#include "panel_ui.h"
#include "panel_theme.h"
#include "lv_port_indev.h"
#include "soc_osal.h"
#include <stdio.h>
#include <string.h>

/* ---- State ---- */
static system_state_t g_state = SYS_STATE_NO_JOB;
static int g_progress = 0;
static uint32_t g_job_seconds = 0;

/* ---- Status bar ---- */
static lv_obj_t *g_lbl_title;
static lv_obj_t *g_lbl_mode;
static lv_obj_t *g_lbl_rx;
static lv_obj_t *g_lbl_sle;
static lv_obj_t *g_lbl_host;

/* ---- Progress block (left) ---- */
static lv_obj_t *g_arc;
static lv_obj_t *g_lbl_pct;
static lv_obj_t *g_lbl_substate;
static lv_obj_t *g_lbl_state_badge;

/* ---- Info block (right) ---- */
static lv_obj_t *g_lbl_job_time;
static lv_obj_t *g_lbl_job_name;
static lv_obj_t *g_lbl_safety_val;
static lv_obj_t *g_lbl_speed;
static lv_obj_t *g_lbl_power;
static lv_obj_t *g_lbl_touch;
static lv_obj_t *g_lbl_ver;

/* ---- Action bar ---- */
static lv_obj_t *g_btn_start;
static lv_obj_t *g_btn_pause;
static lv_obj_t *g_btn_stop;
static lv_obj_t *g_btn_reset;
static lv_obj_t *g_lbl_start;
static lv_obj_t *g_lbl_pause;
static lv_obj_t *g_lbl_stop;
static lv_obj_t *g_lbl_reset;

/* ---- Helpers ---- */
static lv_obj_t *create_label(lv_obj_t *parent, const lv_font_t *font,
                               lv_color_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    return lbl;
}

static lv_obj_t *create_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(card, &style_card, 0);
    return card;
}

/* ---- Status bar ---- */
static void create_status_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 320, 24);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_set_style_pad_gap(bar, 8, 0);
    lv_obj_set_style_bg_color(bar, COLOR_BG_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(bar, 0, 0);

    g_lbl_title = create_label(bar, &lv_font_montserrat_14, COLOR_TEXT_BRIGHT);
    lv_label_set_text(g_lbl_title, "WS63 PANEL");

    g_lbl_mode = create_label(bar, &lv_font_montserrat_10, COLOR_LASER_BLUE);
    lv_label_set_text(g_lbl_mode, "SLE");

    /* Spacer */
    lv_obj_t *spacer = lv_obj_create(bar);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);

    /* Capsules */
    lv_obj_t *caps = lv_obj_create(bar);
    lv_obj_set_size(caps, LV_SIZE_CONTENT, 16);
    lv_obj_remove_flag(caps, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(caps, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(caps, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(caps, 4, 0);
    lv_obj_set_style_pad_all(caps, 0, 0);
    lv_obj_set_style_bg_opa(caps, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(caps, 0, 0);

    g_lbl_rx = create_label(caps, &lv_font_montserrat_10, COLOR_LASER_GREEN);
    lv_label_set_text(g_lbl_rx, "RX OK");

    g_lbl_sle = create_label(caps, &lv_font_montserrat_10, COLOR_TEXT_MUTED);
    lv_label_set_text(g_lbl_sle, "SLE");

    g_lbl_host = create_label(caps, &lv_font_montserrat_10, COLOR_TEXT_MUTED);
    lv_label_set_text(g_lbl_host, "Host");
}

/* ---- Progress arc (left column) ---- */
static void create_progress_block(lv_obj_t *parent)
{
    lv_obj_t *block = create_card(parent, 114, 152);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(block, 4, 0);

    /* Arc container */
    lv_obj_t *arc_cont = lv_obj_create(block);
    lv_obj_set_size(arc_cont, 100, 100);
    lv_obj_remove_flag(arc_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(arc_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(arc_cont, 0, 0);
    lv_obj_set_style_pad_all(arc_cont, 0, 0);

    /* Arc: 90x90, centered */
    g_arc = lv_arc_create(arc_cont);
    lv_obj_set_size(g_arc, 90, 90);
    lv_obj_align(g_arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_bg_angles(g_arc, 0, 360);
    lv_arc_set_angles(g_arc, 0, 0);
    lv_arc_set_mode(g_arc, LV_ARC_MODE_NORMAL);

    /* Track: deep blue-black, width 8 */
    lv_obj_set_style_arc_width(g_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_arc, lv_color_hex(0x0C0F16), LV_PART_MAIN);

    /* Indicator: glowing cyan, width 8, rounded ends */
    lv_obj_set_style_arc_width(g_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_arc, lv_color_hex(0x00FFCC), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g_arc, true, LV_PART_INDICATOR);

    lv_obj_remove_flag(g_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_arc, LV_OBJ_FLAG_HIDDEN);

    /* Percentage: 28px, bold white, centered in arc */
    g_lbl_pct = create_label(arc_cont, &lv_font_montserrat_28, lv_color_white());
    lv_label_set_text(g_lbl_pct, "0%");
    lv_obj_align(g_lbl_pct, LV_ALIGN_CENTER, 0, -10);

    /* Sub-state: 16px, white, below percentage */
    g_lbl_substate = create_label(arc_cont, &lv_font_montserrat_16, lv_color_white());
    lv_label_set_text(g_lbl_substate, "STANDBY");
    lv_obj_align(g_lbl_substate, LV_ALIGN_CENTER, 0, 16);

    /* State badge: 14px, white, below arc container */
    g_lbl_state_badge = create_label(block, &lv_font_montserrat_14, lv_color_white());
    lv_label_set_text(g_lbl_state_badge, "NO_JOB");
}

/* ---- Info block (right column) ---- */
static void create_info_block(lv_obj_t *parent)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_set_size(block, 174, 152);
    lv_obj_remove_flag(block, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(block, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(block, 4, 0);
    lv_obj_set_style_pad_all(block, 0, 0);
    lv_obj_set_style_bg_opa(block, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(block, 0, 0);

    /* Job card */
    lv_obj_t *job_card = create_card(block, 174, 42);
    lv_obj_set_flex_flow(job_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(job_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(job_card, 2, 0);

    /* Timer row: time icon + time value */
    lv_obj_t *time_row = lv_obj_create(job_card);
    lv_obj_set_size(time_row, LV_SIZE_CONTENT, 20);
    lv_obj_remove_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(time_row, 4, 0);
    lv_obj_set_style_pad_all(time_row, 0, 0);
    lv_obj_set_style_bg_opa(time_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(time_row, 0, 0);

    lv_obj_t *time_icon = create_label(time_row, &lv_font_montserrat_10,
                                         lv_color_hex(0x6B7C96));
    lv_label_set_text(time_icon, LV_SYMBOL_REFRESH);

    g_lbl_job_time = create_label(time_row, &lv_font_montserrat_16, COLOR_LASER_GREEN);
    lv_label_set_text(g_lbl_job_time, "00:00");

    g_lbl_job_name = create_label(job_card, &lv_font_montserrat_10, lv_color_hex(0x38BDF8));
    lv_label_set_text(g_lbl_job_name, "NO JOB");
    lv_label_set_long_mode(g_lbl_job_name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(g_lbl_job_name, 160);

    /* Safety card */
    lv_obj_t *safety_card = create_card(block, 174, 28);
    lv_obj_set_flex_flow(safety_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(safety_card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *safety_lbl = create_label(safety_card, &lv_font_montserrat_10,
                                         lv_color_hex(0x6B7C96));
    lv_label_set_text(safety_lbl, "SAFETY");

    g_lbl_safety_val = create_label(safety_card, &lv_font_montserrat_14,
                                     lv_color_hex(0xFFCC00));
    lv_label_set_text(g_lbl_safety_val, "LOCKED");

    /* Params grid */
    lv_obj_t *param_box = create_card(block, 174, 40);
    lv_obj_set_flex_flow(param_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(param_box, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Speed */
    lv_obj_t *speed_col = lv_obj_create(param_box);
    lv_obj_set_size(speed_col, 72, 30);
    lv_obj_remove_flag(speed_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(speed_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(speed_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(speed_col, 0, 0);
    lv_obj_set_style_pad_all(speed_col, 0, 0);
    lv_obj_set_style_bg_opa(speed_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(speed_col, 0, 0);

    lv_obj_t *speed_lbl = create_label(speed_col, &lv_font_montserrat_10,
                                        lv_color_hex(0x6B7C96));
    lv_label_set_text(speed_lbl, "SPEED");
    g_lbl_speed = create_label(speed_col, &lv_font_montserrat_14, lv_color_white());
    lv_label_set_text(g_lbl_speed, "--");

    /* Divider */
    lv_obj_t *div = lv_obj_create(param_box);
    lv_obj_set_size(div, 1, 24);
    lv_obj_set_style_bg_color(div, COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_radius(div, 0, 0);

    /* Power */
    lv_obj_t *power_col = lv_obj_create(param_box);
    lv_obj_set_size(power_col, 72, 30);
    lv_obj_remove_flag(power_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(power_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(power_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(power_col, 0, 0);
    lv_obj_set_style_pad_all(power_col, 0, 0);
    lv_obj_set_style_bg_opa(power_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(power_col, 0, 0);

    lv_obj_t *power_lbl = create_label(power_col, &lv_font_montserrat_10,
                                        lv_color_hex(0x6B7C96));
    lv_label_set_text(power_lbl, "POWER");
    g_lbl_power = create_label(power_col, &lv_font_montserrat_14, lv_color_white());
    lv_label_set_text(g_lbl_power, "--");

    /* Debug row */
    lv_obj_t *debug_row = lv_obj_create(block);
    lv_obj_set_size(debug_row, 174, 16);
    lv_obj_remove_flag(debug_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(debug_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(debug_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(debug_row, 9, 0);
    lv_obj_set_style_pad_ver(debug_row, 5, 0);
    lv_obj_set_style_bg_opa(debug_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(debug_row, 0, 0);

    g_lbl_touch = create_label(debug_row, &lv_font_montserrat_8, COLOR_TEXT_MUTED);
    lv_label_set_text(g_lbl_touch, "Touch: ---");

    g_lbl_ver = create_label(debug_row, &lv_font_montserrat_8, COLOR_TEXT_MUTED);
    lv_label_set_text(g_lbl_ver, "v1.0");
}

/* ---- Button helper ---- */
static lv_obj_t *create_action_btn(lv_obj_t *parent, const char *text,
                                    lv_color_t bg_color, lv_obj_t **out_label)
{
    lv_obj_t *btn = lv_button_create(parent);
    /* Clear theme styles first, then set our own */
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 64, 36);
    lv_obj_set_style_bg_color(btn, bg_color, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_border_width(btn, 0, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);

    if (out_label) *out_label = lbl;
    return btn;
}

/* ---- Action bar ---- */
static void btn_event_cb(lv_event_t *e);
static void create_action_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 320, 48);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 192);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 6, 0);
    lv_obj_set_style_pad_ver(bar, 5, 0);
    lv_obj_set_style_pad_gap(bar, 5, 0);
    lv_obj_set_style_bg_color(bar, COLOR_BG_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(bar, 0, 0);

    g_btn_start = create_action_btn(bar, "START", lv_color_hex(0x10B981), &g_lbl_start);
    lv_obj_add_event_cb(g_btn_start, btn_event_cb, LV_EVENT_CLICKED, (void *)0);

    g_btn_pause = create_action_btn(bar, "PAUSE", lv_color_hex(0xFFB300), &g_lbl_pause);
    lv_obj_add_event_cb(g_btn_pause, btn_event_cb, LV_EVENT_CLICKED, (void *)1);

    g_btn_stop = create_action_btn(bar, "STOP", lv_color_hex(0xFF3366), &g_lbl_stop);
    lv_obj_add_event_cb(g_btn_stop, btn_event_cb, LV_EVENT_CLICKED, (void *)2);

    g_btn_reset = create_action_btn(bar, "RESET", lv_color_hex(0xFF9900), &g_lbl_reset);
    lv_obj_add_event_cb(g_btn_reset, btn_event_cb, LV_EVENT_CLICKED, (void *)3);
}

/* ---- UI update: apply current state to all widgets ---- */
static bool g_btn_start_disabled = false;
static bool g_btn_pause_disabled = false;
static bool g_btn_stop_disabled = false;
static bool g_btn_reset_disabled = false;

static void set_btn_enabled(lv_obj_t *btn, lv_obj_t *lbl, lv_color_t color, bool enabled)
{
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_bg_opa(btn, enabled ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_opa(lbl, enabled ? LV_OPA_COVER : LV_OPA_50, 0);
}

static void apply_state(void)
{
    g_btn_start_disabled = false;
    g_btn_pause_disabled = false;
    g_btn_stop_disabled = false;
    g_btn_reset_disabled = false;

    lv_label_set_text(g_lbl_start, "START");
    lv_label_set_text(g_lbl_pause, "PAUSE");

    switch (g_state) {
    case SYS_STATE_NO_JOB:
        lv_obj_add_flag(g_arc, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_lbl_pct, "0%");
        lv_label_set_text(g_lbl_substate, "STANDBY");
        lv_label_set_text(g_lbl_state_badge, "NO_JOB");
        lv_obj_set_style_text_color(g_lbl_state_badge, lv_color_white(), 0);
        lv_label_set_text(g_lbl_safety_val, "LOCKED");
        lv_obj_set_style_text_color(g_lbl_safety_val, lv_color_hex(0xFFCC00), 0);
        g_btn_start_disabled = true;
        g_btn_pause_disabled = true;
        break;

    case SYS_STATE_RECEIVING:
        lv_obj_add_flag(g_arc, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_lbl_pct, "0%");
        lv_label_set_text(g_lbl_substate, "DOWNLOADING");
        lv_label_set_text(g_lbl_state_badge, "RECEIVING");
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_BLUE, 0);
        lv_label_set_text(g_lbl_safety_val, "LOCKED");
        lv_obj_set_style_text_color(g_lbl_safety_val, lv_color_hex(0xFFCC00), 0);
        g_btn_start_disabled = true;
        g_btn_pause_disabled = true;
        g_btn_reset_disabled = true;
        break;

    case SYS_STATE_READY:
        lv_obj_add_flag(g_arc, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_lbl_pct, "0%");
        lv_label_set_text(g_lbl_substate, "STANDBY");
        lv_label_set_text(g_lbl_state_badge, "READY");
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_GREEN, 0);
        lv_label_set_text(g_lbl_safety_val, "OFF");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_GREEN, 0);
        g_btn_pause_disabled = true;
        break;

    case SYS_STATE_RUNNING:
        lv_obj_clear_flag(g_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_arc_color(g_arc, COLOR_LASER_GREEN, LV_PART_INDICATOR);
        lv_arc_set_value(g_arc, g_progress);
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", g_progress);
            lv_label_set_text(g_lbl_pct, buf);
        }
        lv_label_set_text(g_lbl_substate, "ENGRAVING");
        lv_label_set_text(g_lbl_state_badge, "RUNNING");
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_RED, 0);
        lv_label_set_text(g_lbl_safety_val, "ON");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_RED, 0);
        g_btn_start_disabled = true;
        g_btn_reset_disabled = true;
        break;

    case SYS_STATE_PAUSED:
        lv_obj_clear_flag(g_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_arc_color(g_arc, COLOR_LASER_YELLOW, LV_PART_INDICATOR);
        lv_arc_set_value(g_arc, g_progress);
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", g_progress);
            lv_label_set_text(g_lbl_pct, buf);
        }
        lv_label_set_text(g_lbl_substate, "PAUSED");
        lv_label_set_text(g_lbl_state_badge, "PAUSED");
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_YELLOW, 0);
        lv_label_set_text(g_lbl_safety_val, "OFF");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_GREEN, 0);
        lv_label_set_text(g_lbl_start, "RESUME");
        break;

    case SYS_STATE_DONE:
        lv_obj_clear_flag(g_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_arc_color(g_arc, COLOR_LASER_GREEN, LV_PART_INDICATOR);
        lv_arc_set_value(g_arc, 100);
        lv_label_set_text(g_lbl_pct, "100%");
        lv_label_set_text(g_lbl_substate, "COMPLETED");
        lv_label_set_text(g_lbl_state_badge, "DONE");
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_GREEN, 0);
        lv_label_set_text(g_lbl_safety_val, "OFF");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_GREEN, 0);
        lv_label_set_text(g_lbl_start, "REPEAT");
        g_btn_pause_disabled = true;
        break;

    case SYS_STATE_ERROR:
        lv_obj_add_flag(g_arc, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_lbl_pct, "0%");
        lv_label_set_text(g_lbl_substate, "ALARM");
        lv_label_set_text(g_lbl_state_badge, "ERROR");
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_RED, 0);
        lv_label_set_text(g_lbl_safety_val, "LOCKED");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_RED, 0);
        g_btn_start_disabled = true;
        g_btn_pause_disabled = true;
        break;

    case SYS_STATE_LINK_LOST:
        lv_obj_add_flag(g_arc, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_lbl_pct, "0%");
        lv_label_set_text(g_lbl_substate, "LINK LOST");
        lv_label_set_text(g_lbl_state_badge, "RX LOST");
        lv_obj_set_style_text_color(g_lbl_state_badge, lv_color_hex(0x5A6578), 0);
        lv_label_set_text(g_lbl_safety_val, "UNKNOWN");
        lv_obj_set_style_text_color(g_lbl_safety_val, lv_color_hex(0x5A6578), 0);
        g_btn_start_disabled = true;
        g_btn_pause_disabled = true;
        g_btn_stop_disabled = true;
        g_btn_reset_disabled = true;
        break;

    default:
        break;
    }

    set_btn_enabled(g_btn_start, g_lbl_start, lv_color_hex(0x10B981), !g_btn_start_disabled);
    set_btn_enabled(g_btn_pause, g_lbl_pause, lv_color_hex(0xFFB300), !g_btn_pause_disabled);
    set_btn_enabled(g_btn_stop, g_lbl_stop, lv_color_hex(0xFF3366), !g_btn_stop_disabled);
    set_btn_enabled(g_btn_reset, g_lbl_reset, lv_color_hex(0xFF9900), !g_btn_reset_disabled);
}

/* ---- Button callbacks ---- */
static void btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    osal_printk("[PANEL] btn %lu clicked, state=%d\r\n", (unsigned long)idx, g_state);

    switch (idx) {
    case 0: /* Start */
        if (g_state == SYS_STATE_READY) {
            panel_ui_set_state(SYS_STATE_RUNNING);
        } else if (g_state == SYS_STATE_PAUSED) {
            panel_ui_set_state(SYS_STATE_RUNNING);
        } else if (g_state == SYS_STATE_DONE) {
            g_progress = 0;
            panel_ui_set_state(SYS_STATE_RUNNING);
        }
        break;
    case 1: /* Pause */
        if (g_state == SYS_STATE_RUNNING) {
            panel_ui_set_state(SYS_STATE_PAUSED);
        }
        break;
    case 2: /* Stop - always available except LINK_LOST */
        if (g_state != SYS_STATE_LINK_LOST) {
            g_progress = 0;
            panel_ui_set_state(SYS_STATE_ERROR);
        }
        break;
    case 3: /* Reset */
        if (g_state == SYS_STATE_ERROR) {
            panel_ui_set_state(SYS_STATE_NO_JOB);
        } else if (g_state == SYS_STATE_DONE || g_state == SYS_STATE_READY ||
                   g_state == SYS_STATE_PAUSED) {
            g_progress = 0;
            g_job_seconds = 0;
            panel_ui_set_job_time(0);
            panel_ui_set_state(SYS_STATE_NO_JOB);
        }
        break;
    }
}

/* ---- Touch callback ---- */
static void touch_handler(bool pressed, int16_t x, int16_t y)
{
    char buf[24];
    if (pressed) {
        snprintf(buf, sizeof(buf), "Touch:%3d,%3d", x, y);
    } else {
        snprintf(buf, sizeof(buf), "Touch: ---");
    }
    lv_label_set_text(g_lbl_touch, buf);
}

/* ---- Timer for job time ---- */
static void job_time_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_state == SYS_STATE_RUNNING) {
        g_job_seconds++;
        panel_ui_set_job_time(g_job_seconds);
    }
}

/* ---- Public API ---- */
void panel_ui_create(void)
{
    panel_theme_init();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_remove_style_all(scr);
    lv_obj_add_style(scr, &style_screen, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    create_status_bar(scr);

    /* Body container */
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_set_size(body, 320, 168);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 24);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(body, 8, 0);
    lv_obj_set_style_pad_left(body, 8, 0);
    lv_obj_set_style_pad_top(body, 8, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);

    create_progress_block(body);
    create_info_block(body);

    create_action_bar(scr);

    /* Register touch callback */
    g_panel_touch_cb = touch_handler;

    /* Job time timer (1 second) */
    lv_timer_create(job_time_timer_cb, 1000, NULL);

    /* Initial state */
    apply_state();

    osal_printk("[PANEL] UI created\r\n");
}

void panel_ui_set_state(system_state_t state)
{
    if (state >= SYS_STATE_COUNT) return;
    g_state = state;
    apply_state();
}

system_state_t panel_ui_get_state(void)
{
    return g_state;
}

void panel_ui_set_progress(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_progress = pct;

    if (g_state == SYS_STATE_RUNNING || g_state == SYS_STATE_PAUSED ||
        g_state == SYS_STATE_DONE) {
        lv_arc_set_value(g_arc, pct);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(g_lbl_pct, buf);
    }
}

void panel_ui_set_job_name(const char *name)
{
    if (name) {
        lv_label_set_text(g_lbl_job_name, name);
    }
}

void panel_ui_set_job_time(uint32_t seconds)
{
    uint32_t min = seconds / 60;
    uint32_t sec = seconds % 60;
    char buf[12];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)min, (unsigned long)sec);
    lv_label_set_text(g_lbl_job_time, buf);
}

void panel_ui_set_safety(const char *text, lv_color_t color)
{
    lv_label_set_text(g_lbl_safety_val, text);
    lv_obj_set_style_text_color(g_lbl_safety_val, color, 0);
}

void panel_ui_set_speed(const char *text)
{
    lv_label_set_text(g_lbl_speed, text);
}

void panel_ui_set_power(const char *text)
{
    lv_label_set_text(g_lbl_power, text);
}

void panel_ui_set_rx_status(bool connected)
{
    lv_obj_set_style_text_color(g_lbl_rx,
                                 connected ? COLOR_LASER_GREEN : COLOR_LASER_RED, 0);
    lv_label_set_text(g_lbl_rx, connected ? "RX OK" : "RX LOST");
}

void panel_ui_set_sle_status(bool connected)
{
    lv_obj_set_style_text_color(g_lbl_sle,
                                 connected ? COLOR_LASER_BLUE : COLOR_TEXT_MUTED, 0);
    lv_label_set_text(g_lbl_sle, connected ? "SLE" : "SLE");
}

void panel_ui_set_host_status(bool connected)
{
    lv_obj_set_style_text_color(g_lbl_host,
                                 connected ? COLOR_LASER_GREEN : COLOR_TEXT_MUTED, 0);
    lv_label_set_text(g_lbl_host, connected ? "Host" : "Host");
}

void panel_ui_update_touch(bool pressed, int16_t x, int16_t y)
{
    (void)pressed;
    (void)x;
    (void)y;
}
