/**
 * @file home_page.c
 * @brief Home Dashboard: status bar + progress arc + info panel + action buttons.
 *
 * Layout (320x240 landscape):
 *   status_bar   (0,0)   320x32
 *   body         (0,32)  320x160
 *     progress_block  (8,32)   114x152
 *     info_block       (130,32) 174x152
 *   action_bar   (0,192) 320x48
 */
#include "home_page.h"
#include "panel_theme.h"
#include "ui_manager.h"
#include "../service/panel_model.h"
#include "soc_osal.h"
#include <stdio.h>
#include <string.h>

/* Status bar */
static lv_obj_t *g_lbl_rx;
static lv_obj_t *g_lbl_sle;

/* Progress block */
static lv_obj_t *g_lbl_pct;
static lv_obj_t *g_lbl_substate;
static lv_obj_t *g_lbl_state_badge;

/* Info block */
static lv_obj_t *g_lbl_job_time;
static lv_obj_t *g_lbl_job_name;
static lv_obj_t *g_lbl_safety_val;
static lv_obj_t *g_lbl_speed;
static lv_obj_t *g_lbl_power;

/* Action bar buttons */
static lv_obj_t *g_btn_start;
static lv_obj_t *g_btn_stop;
static lv_obj_t *g_btn_abort;
static lv_obj_t *g_btn_focus;
static lv_obj_t *g_btn_settings;
static lv_obj_t *g_lbl_start;
static lv_obj_t *g_lbl_stop;
static lv_obj_t *g_lbl_abort;
static lv_obj_t *g_lbl_focus;
static lv_obj_t *g_lbl_settings;
static bool g_focus_visual_on;
static bool g_focus_visual_allowed;

static lv_obj_t *create_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
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

static void status_bar_click_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_switch_page(PAGE_DIAGNOSTICS);
}

static void create_status_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 320, 32);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_set_style_pad_gap(bar, 8, 0);
    lv_obj_set_style_bg_color(bar, COLOR_BG_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_add_event_cb(bar, status_bar_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = create_label(bar, PANEL_FONT_CN, COLOR_TEXT_BRIGHT);
    lv_label_set_text(title, "WS63 激光面板");

    lv_obj_t *spacer = lv_obj_create(bar);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);

    lv_obj_t *caps = lv_obj_create(bar);
    lv_obj_set_size(caps, 104, 24);
    lv_obj_remove_flag(caps, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(caps, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(caps, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(caps, 4, 0);
    lv_obj_set_style_pad_hor(caps, 4, 0);
    lv_obj_set_style_pad_ver(caps, 0, 0);
    lv_obj_set_style_bg_color(caps, lv_color_hex(0x0C111B), 0);
    lv_obj_set_style_bg_opa(caps, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(caps, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(caps, 1, 0);
    lv_obj_set_style_radius(caps, 8, 0);
    lv_obj_add_flag(caps, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(caps, 4);
    lv_obj_add_event_cb(caps, status_bar_click_cb, LV_EVENT_CLICKED, NULL);

    g_lbl_rx = create_label(caps, PANEL_FONT_CN, COLOR_LASER_GREEN);
    lv_obj_set_width(g_lbl_rx, 54);
    lv_obj_set_style_text_align(g_lbl_rx, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(g_lbl_rx, "RX正常");

    g_lbl_sle = create_label(caps, &lv_font_montserrat_10, COLOR_LASER_BLUE);
    lv_obj_set_width(g_lbl_sle, 36);
    lv_obj_set_style_text_align(g_lbl_sle, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(g_lbl_sle, "SLE");
}

static void create_progress_block(lv_obj_t *parent)
{
    lv_obj_t *block = create_card(parent, 114, 152);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(block, 8, 0);

    lv_obj_t *progress_title = create_label(block, PANEL_FONT_CN, COLOR_TEXT_MUTED);
    lv_label_set_text(progress_title, "任务进度");

    g_lbl_pct = create_label(block, &lv_font_montserrat_28, lv_color_white());
    lv_label_set_text(g_lbl_pct, "0%");

    g_lbl_substate = create_label(block, PANEL_FONT_CN, lv_color_white());
    lv_label_set_text(g_lbl_substate, "待机");
    lv_obj_set_width(g_lbl_substate, 100);
    lv_obj_set_style_text_align(g_lbl_substate, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_lbl_substate, LV_LABEL_LONG_CLIP);

    g_lbl_state_badge = create_label(block, PANEL_FONT_CN, lv_color_white());
    lv_label_set_text(g_lbl_state_badge, "无任务");
}

static void create_info_block(lv_obj_t *parent)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_set_size(block, 174, 152);
    lv_obj_remove_flag(block, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(block, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(block, 4, 0);
    lv_obj_set_style_pad_all(block, 0, 0);
    lv_obj_set_style_bg_opa(block, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(block, 0, 0);

    lv_obj_t *job_card = create_card(block, 174, 48);
    lv_obj_set_flex_flow(job_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(job_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(job_card, 2, 0);

    lv_obj_t *time_row = lv_obj_create(job_card);
    lv_obj_set_size(time_row, LV_SIZE_CONTENT, 20);
    lv_obj_remove_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(time_row, 4, 0);
    lv_obj_set_style_pad_all(time_row, 0, 0);
    lv_obj_set_style_bg_opa(time_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(time_row, 0, 0);

    lv_obj_t *time_icon = create_label(time_row, &lv_font_montserrat_10, COLOR_TEXT_MUTED);
    lv_label_set_text(time_icon, LV_SYMBOL_REFRESH);

    g_lbl_job_time = create_label(time_row, &lv_font_montserrat_16, COLOR_LASER_GREEN);
    lv_label_set_text(g_lbl_job_time, "00:00");

    g_lbl_job_name = create_label(job_card, PANEL_FONT_CN, COLOR_LASER_BLUE);
    lv_label_set_text(g_lbl_job_name, "暂无任务");
    lv_label_set_long_mode(g_lbl_job_name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(g_lbl_job_name, 160);

    lv_obj_t *safety_card = create_card(block, 174, 32);
    lv_obj_set_flex_flow(safety_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(safety_card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *safety_lbl = create_label(safety_card, PANEL_FONT_CN, COLOR_TEXT_MUTED);
    lv_label_set_text(safety_lbl, "激光状态");

    g_lbl_safety_val = create_label(safety_card, PANEL_FONT_CN, COLOR_LASER_YELLOW);
    lv_label_set_text(g_lbl_safety_val, "已锁定");

    lv_obj_t *param_box = create_card(block, 174, 44);
    lv_obj_set_flex_flow(param_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(param_box, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *speed_col = lv_obj_create(param_box);
    lv_obj_set_size(speed_col, 72, 30);
    lv_obj_remove_flag(speed_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(speed_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(speed_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(speed_col, 0, 0);
    lv_obj_set_style_pad_all(speed_col, 0, 0);
    lv_obj_set_style_bg_opa(speed_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(speed_col, 0, 0);

    lv_obj_t *speed_lbl = create_label(speed_col, PANEL_FONT_CN, COLOR_TEXT_MUTED);
    lv_label_set_text(speed_lbl, "速度");
    g_lbl_speed = create_label(speed_col, &lv_font_montserrat_14, lv_color_white());
    lv_label_set_text(g_lbl_speed, "--");

    lv_obj_t *div = lv_obj_create(param_box);
    lv_obj_set_size(div, 1, 24);
    lv_obj_set_style_bg_color(div, COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_radius(div, 0, 0);

    lv_obj_t *power_col = lv_obj_create(param_box);
    lv_obj_set_size(power_col, 72, 30);
    lv_obj_remove_flag(power_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(power_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(power_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(power_col, 0, 0);
    lv_obj_set_style_pad_all(power_col, 0, 0);
    lv_obj_set_style_bg_opa(power_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(power_col, 0, 0);

    lv_obj_t *power_lbl = create_label(power_col, PANEL_FONT_CN, COLOR_TEXT_MUTED);
    lv_label_set_text(power_lbl, "功率");
    g_lbl_power = create_label(power_col, &lv_font_montserrat_14, lv_color_white());
    lv_label_set_text(g_lbl_power, "--");
}

static void set_focus_visual_state(bool on)
{
    g_focus_visual_on = on;
    lv_obj_set_style_bg_color(g_btn_focus,
        on ? COLOR_LASER_RED : COLOR_LASER_BLUE, 0);
    lv_label_set_text(g_lbl_focus, on ? "调焦开启" : "调焦");
}

static void btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    panel_button_permissions_t perms;
    panel_model_get_button_permissions(&perms);

    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    switch (idx) {
    case 0:
        if (!perms.can_start) {
            osal_printk("[PANEL_CMD] start rejected: owner=%d state=%d\r\n",
                        g_model.owner, g_model.state);
            break;
        }
        panel_model_set_scene(PANEL_SCENE_SCREEN_SENDING);
        osal_printk("[PANEL_CMD] demo start request -> SCREEN_SENDING (backend not connected)\r\n");
        break;
    case 1:
        if (!perms.can_stop) {
            osal_printk("[PANEL_CMD] stop rejected: state=%d\r\n", g_model.state);
            break;
        }
        panel_model_request_stop();
        osal_printk("[PANEL_CMD] stop request queued in fake model\r\n");
        break;
    case 2:
        if (!perms.can_abort) {
            osal_printk("[PANEL_CMD] abort rejected: state=%d\r\n", g_model.state);
            break;
        }
        panel_model_request_abort();
        osal_printk("[PANEL_CMD] abort request queued in fake model\r\n");
        break;
    case 3:
        if (!perms.can_focus_off) {
            osal_printk("[PANEL_CMD] focus_off rejected by UI state\r\n");
            break;
        }
        panel_model_request_focus_off();
        osal_printk("[PANEL_CMD] focus_off request queued in fake model\r\n");
        break;
    case 4: ui_manager_switch_page(PAGE_SETTINGS); break;
    }
}

static lv_obj_t *create_action_btn(lv_obj_t *parent, const char *text,
                                    lv_color_t bg_color, lv_obj_t **out_label)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 58, 40);
    lv_obj_set_style_bg_color(btn, bg_color, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 1);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);

    if (out_label) *out_label = lbl;
    return btn;
}

static void create_action_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 320, 48);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 192);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 4, 0);
    lv_obj_set_style_pad_ver(bar, 4, 0);
    lv_obj_set_style_pad_gap(bar, 2, 0);
    lv_obj_set_style_bg_color(bar, COLOR_BG_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(bar, 0, 0);

    g_btn_start = create_action_btn(bar, "启动", COLOR_LASER_GREEN, &g_lbl_start);
    lv_obj_add_event_cb(g_btn_start, btn_event_cb, LV_EVENT_CLICKED, (void *)0);

    g_btn_stop = create_action_btn(bar, "停止", COLOR_LASER_RED, &g_lbl_stop);
    lv_obj_add_event_cb(g_btn_stop, btn_event_cb, LV_EVENT_CLICKED, (void *)1);

    g_btn_abort = create_action_btn(bar, "中止", COLOR_LASER_ORANGE, &g_lbl_abort);
    lv_obj_add_event_cb(g_btn_abort, btn_event_cb, LV_EVENT_CLICKED, (void *)2);

    g_btn_focus = create_action_btn(bar, "调焦", COLOR_LASER_BLUE, &g_lbl_focus);
    lv_obj_add_event_cb(g_btn_focus, btn_event_cb, LV_EVENT_CLICKED, (void *)3);

    g_btn_settings = create_action_btn(bar, "设置", COLOR_TEXT_MUTED, &g_lbl_settings);
    lv_obj_add_event_cb(g_btn_settings, btn_event_cb, LV_EVENT_CLICKED, (void *)4);
}

static void apply_state(void)
{
    panel_button_permissions_t perms;
    panel_model_get_button_permissions(&perms);
    bool start_en = false, stop_en = false, abort_en = false, focus_en = false;

    lv_label_set_text(g_lbl_substate, "待机");

    switch (g_model.state) {
    case SYS_STATE_NO_JOB:
        lv_label_set_text(g_lbl_pct, "0%");
        lv_label_set_text(g_lbl_state_badge, "无任务");
        lv_obj_set_style_text_color(g_lbl_state_badge, lv_color_white(), 0);
        lv_label_set_text(g_lbl_safety_val, "已锁定");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_YELLOW, 0);
        break;

    case SYS_STATE_BROWSING:
        lv_label_set_text(g_lbl_pct, "0%");
        lv_label_set_text(g_lbl_substate, "暂无任务");
        lv_label_set_text(g_lbl_state_badge, "暂无任务");
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_BLUE, 0);
        lv_label_set_text(g_lbl_safety_val, "关闭");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_GREEN, 0);
        break;

    case SYS_STATE_RECEIVING:
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", g_model.progress);
            lv_label_set_text(g_lbl_pct, buf);
        }
        lv_label_set_text(g_lbl_substate, "接收中");
        lv_label_set_text(g_lbl_state_badge, "正在接收");
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_BLUE, 0);
        lv_label_set_text(g_lbl_safety_val, "已锁定");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_YELLOW, 0);
        break;

    case SYS_STATE_SENDING:
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", g_model.progress);
            lv_label_set_text(g_lbl_pct, buf);
        }
        lv_label_set_text(g_lbl_substate, "任务发送");
        lv_label_set_text(g_lbl_state_badge, "发送中");
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_BLUE, 0);
        lv_label_set_text(g_lbl_safety_val, "关闭");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_GREEN, 0);
        break;

    case SYS_STATE_READY:
        lv_label_set_text(g_lbl_pct, "0%");
        lv_label_set_text(g_lbl_state_badge, "就绪");
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_GREEN, 0);
        lv_label_set_text(g_lbl_safety_val, "关闭");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_GREEN, 0);
        break;

    case SYS_STATE_RUNNING:
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", g_model.progress);
            lv_label_set_text(g_lbl_pct, buf);
        }
        lv_label_set_text(g_lbl_substate, "加工中");
        lv_label_set_text(g_lbl_state_badge, "运行中");
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_RED, 0);
        lv_label_set_text(g_lbl_safety_val,
            g_model.laser_output_active ? "激光中" : "关闭");
        lv_obj_set_style_text_color(g_lbl_safety_val,
            g_model.laser_output_active ? COLOR_LASER_RED : COLOR_LASER_GREEN, 0);
        break;

    case SYS_STATE_DONE:
        lv_label_set_text(g_lbl_pct, "100%");
        lv_label_set_text(g_lbl_substate, "已完成");
        lv_label_set_text(g_lbl_state_badge, "完成");
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_GREEN, 0);
        lv_label_set_text(g_lbl_safety_val, "关闭");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_GREEN, 0);
        break;

    case SYS_STATE_REQUESTING_STOP:
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", g_model.progress);
            lv_label_set_text(g_lbl_pct, buf);
        }
        lv_label_set_text(g_lbl_substate, "停止中");
        lv_label_set_text(g_lbl_state_badge, "STOP中");
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_ORANGE, 0);
        lv_label_set_text(g_lbl_safety_val, "待执行");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_YELLOW, 0);
        break;

    case SYS_STATE_REQUESTING_ABORT:
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", g_model.progress);
            lv_label_set_text(g_lbl_pct, buf);
        }
        lv_label_set_text(g_lbl_substate, "中止中");
        lv_label_set_text(g_lbl_state_badge, "ABORT中");
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_ORANGE, 0);
        lv_label_set_text(g_lbl_safety_val, "待执行");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_YELLOW, 0);
        break;

    case SYS_STATE_REQUESTING_FOCUS_OFF:
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", g_model.progress);
            lv_label_set_text(g_lbl_pct, buf);
        }
        lv_label_set_text(g_lbl_substate, "关光中");
        lv_label_set_text(g_lbl_state_badge, "关光中");
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_ORANGE, 0);
        lv_label_set_text(g_lbl_safety_val, "待执行");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_YELLOW, 0);
        break;

    case SYS_STATE_ERROR:
        lv_label_set_text(g_lbl_pct, "0%");
        lv_label_set_text(g_lbl_substate, "告警");
        lv_label_set_text(g_lbl_state_badge, "错误");
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_RED, 0);
        lv_label_set_text(g_lbl_safety_val, "已锁定");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_RED, 0);
        abort_en = true;
        break;

    case SYS_STATE_LINK_LOST:
        lv_label_set_text(g_lbl_pct, "0%");
        lv_label_set_text(g_lbl_substate, "链路断开");
        lv_label_set_text(g_lbl_state_badge, "RX断开");
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_TEXT_MUTED, 0);
        lv_label_set_text(g_lbl_safety_val, "未知");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_TEXT_MUTED, 0);
        break;

    default:
        break;
    }

    start_en = perms.can_start;
    stop_en = perms.can_stop;
    abort_en = perms.can_abort;
    focus_en = perms.can_focus_off;

    {
        char time_buf[12];
        uint32_t min = g_model.job_seconds / 60;
        uint32_t sec = g_model.job_seconds % 60;
        snprintf(time_buf, sizeof(time_buf), "%02lu:%02lu", (unsigned long)min, (unsigned long)sec);
        lv_label_set_text(g_lbl_job_time, time_buf);
    }
    lv_label_set_text(g_lbl_job_name, g_model.job_name);

    lv_obj_set_style_text_color(g_lbl_rx,
        g_model.rx_connected ? COLOR_LASER_GREEN : COLOR_LASER_RED, 0);
    lv_label_set_text(g_lbl_rx, g_model.rx_connected ? "RX正常" : "RX断开");
    lv_obj_set_style_text_color(g_lbl_sle,
        g_model.sle_connected ? COLOR_LASER_BLUE : COLOR_TEXT_MUTED, 0);
    lv_label_set_text(g_lbl_sle, panel_model_owner_text(g_model.owner));

    {
        char speed_buf[12];
        char power_buf[12];
        snprintf(speed_buf, sizeof(speed_buf), "%s",
                 g_model.state == SYS_STATE_RUNNING ? "F1000" : "--");
        snprintf(power_buf, sizeof(power_buf), "%s",
                 g_model.laser_output_active ? "S500" : "--");
        lv_label_set_text(g_lbl_speed, speed_buf);
        lv_label_set_text(g_lbl_power, power_buf);
    }

    set_focus_visual_state(g_model.focus_active);
    g_focus_visual_allowed = focus_en;

    lv_obj_set_style_bg_opa(g_btn_start, start_en ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_text_opa(g_lbl_start, start_en ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_bg_opa(g_btn_stop, stop_en ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_text_opa(g_lbl_stop, stop_en ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_bg_opa(g_btn_abort, abort_en ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_text_opa(g_lbl_abort, abort_en ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_bg_opa(g_btn_focus, focus_en ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_text_opa(g_lbl_focus, focus_en ? LV_OPA_COVER : LV_OPA_50, 0);

    lv_label_set_text(g_lbl_start, perms.can_start ? "启动" : "启动");
    lv_label_set_text(g_lbl_stop, perms.requesting_stop ? "停止中" : "停止");
    lv_label_set_text(g_lbl_abort, perms.requesting_abort ? "中止中" : "中止");
    if (perms.requesting_focus_off) {
        lv_label_set_text(g_lbl_focus, "关光中");
    } else if (g_model.focus_active) {
        lv_label_set_text(g_lbl_focus, "关调焦");
    } else {
        lv_label_set_text(g_lbl_focus, "关光");
    }
}

void home_page_create(lv_obj_t *parent)
{
    lv_obj_t *scr = parent;
    lv_obj_remove_style_all(scr);
    lv_obj_add_style(scr, &style_screen, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    create_status_bar(scr);

    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_set_size(body, 320, 160);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 32);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(body, 8, 0);
    lv_obj_set_style_pad_left(body, 8, 0);
    lv_obj_set_style_pad_top(body, 8, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);

    create_progress_block(body);
    create_info_block(body);
    create_action_bar(scr);
    g_focus_visual_on = false;
    g_focus_visual_allowed = false;
}

void home_page_update(void)
{
    apply_state();
}
