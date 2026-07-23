/**
 * @file home_page.c
 * @brief Home Dashboard: status bar + progress arc + info panel + action buttons.
 *
 * Layout (320x240 landscape):
 *   status_bar   (0,0)   320x32
 *   body         (0,32)  320x152
 *     progress_block  (8,32)   114x152
 *     info_block       (130,32) 174x152
 *   action_bar   (0,184) 320x56
 */
#include "home_page.h"
#include "panel_theme.h"
#include "ui_manager.h"
#include "../service/panel_file_manager.h"
#include "../service/panel_model.h"
#include "../service/panel_offline_job.h"
#include "../service/panel_rx_commands.h"
#include "../service/panel_transport_sle.h"
#include "soc_osal.h"
#include <stdio.h>
#include <string.h>

/* Status bar */
static lv_obj_t *g_title_dot;
static lv_obj_t *g_lbl_title;
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
static lv_obj_t *g_lbl_sd_file;

/* Action bar buttons */
static lv_obj_t *g_btn_start;
static lv_obj_t *g_btn_pause;
static lv_obj_t *g_btn_resume;
static lv_obj_t *g_btn_force_stop;
static lv_obj_t *g_btn_settings;
static lv_obj_t *g_lbl_start;
static lv_obj_t *g_lbl_pause;
static lv_obj_t *g_lbl_resume;
static lv_obj_t *g_lbl_force_stop;
static lv_obj_t *g_lbl_settings;
static uint32_t g_rendered_model_seq = UINT32_MAX;
static uint32_t g_rendered_file_seq = UINT32_MAX;

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

static void bind_click(lv_obj_t *obj, lv_event_cb_t cb, void *user_data)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(obj, 4);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, user_data);
}

static void status_bar_click_cb(lv_event_t *e)
{
    (void)e;
    osal_printk("[HOME] status click -> diagnostics\r\n");
    ui_manager_switch_page(PAGE_DIAGNOSTICS);
}

static void create_status_bar(lv_obj_t *parent, const panel_model_t *model)
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

    g_title_dot = lv_obj_create(bar);
    lv_obj_remove_style_all(g_title_dot);
    lv_obj_set_size(g_title_dot, 8, 8);
    lv_obj_set_style_bg_color(g_title_dot, COLOR_LASER_BLUE, 0);
    lv_obj_set_style_bg_opa(g_title_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_title_dot, LV_RADIUS_CIRCLE, 0);

    g_lbl_title = create_label(bar, PANEL_FONT_CN, COLOR_TEXT_BRIGHT);
    lv_label_set_text(g_lbl_title, "WS63 激光主控");
    lv_obj_set_style_text_color(g_lbl_title,
        model->view_mode == PANEL_VIEW_OFFLINE ? COLOR_LASER_YELLOW : COLOR_LASER_BLUE, 0);

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
    lv_label_set_text(g_lbl_rx, "TX正常");

    g_lbl_sle = create_label(caps, PANEL_FONT_CN, COLOR_LASER_BLUE);
    lv_obj_set_width(g_lbl_sle, 36);
    lv_obj_set_style_text_align(g_lbl_sle, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(g_lbl_sle, "SLE");
}

static void create_progress_block(lv_obj_t *parent, const panel_model_t *model)
{
    lv_obj_t *block = create_card(parent, 114, 152);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(block, 8, 0);

    lv_obj_t *progress_title = create_label(block, PANEL_FONT_CN, COLOR_TEXT_MUTED);
    lv_label_set_text(progress_title, "任务进度");

    g_lbl_pct = create_label(block, &lv_font_montserrat_28, COLOR_TEXT_BRIGHT);
    lv_label_set_text(g_lbl_pct, "0%");

    g_lbl_substate = create_label(block, PANEL_FONT_CN, COLOR_TEXT_MUTED);
    lv_label_set_text(g_lbl_substate, panel_model_state_detail(model->state));
    lv_obj_set_width(g_lbl_substate, 100);
    lv_obj_set_style_text_align(g_lbl_substate, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_lbl_substate, LV_LABEL_LONG_CLIP);

    g_lbl_state_badge = create_label(block, PANEL_FONT_CN, COLOR_TEXT_BRIGHT);
    lv_label_set_text(g_lbl_state_badge, "无任务");
}

static void sd_file_card_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        panel_model_t model;
        panel_model_get_snapshot(&model);
        if (model.view_mode == PANEL_VIEW_ONLINE) {
            /* Opening SD tasks is an explicit authority switch, not merely a
             * page navigation.  The transport will claim RX after TX is
             * safely released. */
            panel_model_toggle_primary_mode();
            panel_model_get_snapshot(&model);
            if (model.view_mode != PANEL_VIEW_OFFLINE) {
                osal_printk("[HOME] SD task rejected: online job still active\r\n");
                return;
            }
        }
        osal_printk("[HOME] sd file click -> offline file browser\r\n");
        ui_manager_switch_page(PAGE_FILE_BROWSER);
    }
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
    lv_label_set_long_mode(g_lbl_job_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_lbl_job_name, 160);

    lv_obj_t *safety_card = create_card(block, 174, 32);
    lv_obj_set_flex_flow(safety_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(safety_card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *safety_lbl = create_label(safety_card, PANEL_FONT_CN, COLOR_TEXT_MUTED);
    lv_label_set_text(safety_lbl, "激光状态");

    g_lbl_safety_val = create_label(safety_card, PANEL_FONT_CN, COLOR_LASER_YELLOW);
    lv_label_set_text(g_lbl_safety_val, "关闭");

    lv_obj_t *file_card = create_card(block, 174, 44);
    lv_obj_set_flex_flow(file_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(file_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(file_card, 6, 0);
    bind_click(file_card, sd_file_card_cb, NULL);

    lv_obj_t *file_lbl = create_label(file_card, PANEL_FONT_CN, COLOR_TEXT_MUTED);
    lv_label_set_text(file_lbl, "SD任务");
    lv_obj_set_width(file_lbl, 48);
    lv_label_set_long_mode(file_lbl, LV_LABEL_LONG_CLIP);
    bind_click(file_lbl, sd_file_card_cb, NULL);

    g_lbl_sd_file = create_label(file_card, PANEL_FONT_CN, COLOR_LASER_BLUE);
    lv_obj_set_width(g_lbl_sd_file, 104);
    lv_label_set_long_mode(g_lbl_sd_file, LV_LABEL_LONG_DOT);
    lv_label_set_text(g_lbl_sd_file, "点击选择");
    bind_click(g_lbl_sd_file, sd_file_card_cb, NULL);
}

static void btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    panel_button_permissions_t perms;
    panel_model_get_button_permissions(&perms);
    panel_model_t g_model;
    panel_model_get_snapshot(&g_model);

    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    osal_printk("[HOME] action click idx=%u owner=%d mode=%d state=%d start=%d stop=%d abort=%d\r\n",
                (unsigned int)idx, g_model.owner, g_model.mode, g_model.state,
                perms.can_start, perms.can_stop, perms.can_abort);
    switch (idx) {
    case 0:
        if (!perms.can_start || g_model.state == SYS_STATE_PAUSED) {
            osal_printk("[PANEL_CMD] start rejected: owner=%d state=%d\r\n",
                        g_model.owner, g_model.state);
            break;
        }
        if (g_model.view_mode == PANEL_VIEW_OFFLINE &&
            (g_model.state == SYS_STATE_NO_JOB || g_model.state == SYS_STATE_BROWSING ||
             g_model.state == SYS_STATE_DONE)) {
            ui_manager_switch_page(PAGE_FILE_BROWSER);
            break;
        }
        if (g_model.view_mode == PANEL_VIEW_OFFLINE &&
            g_model.owner == PANEL_OWNER_SCREEN &&
            g_model.state == SYS_STATE_READY) {
            errcode_t ret = panel_offline_job_start_selected();
            if (ret != ERRCODE_SUCC) {
                osal_printk("[PANEL_CMD] offline start failed: 0x%x\r\n", ret);
                break;
            }
            osal_printk("[PANEL_CMD] offline job queued\r\n");
            break;
        }
        osal_printk("[PANEL_CMD] start ignored: no selected standalone job tx=%d owner=%d state=%d\r\n",
                    panel_transport_sle_tx_is_connected(), g_model.owner, g_model.state);
        break;
    case 1:
        if (!perms.can_stop) {
            osal_printk("[PANEL_CMD] stop rejected: state=%d\r\n", g_model.state);
            break;
        }
        if (panel_rx_commands_request_exec_stop() == ERRCODE_SUCC) {
            panel_model_request_stop();
        } else {
            osal_printk("[PANEL_CMD] stop queue failed\r\n");
        }
        break;
    case 2:
        if (!perms.can_start || g_model.state != SYS_STATE_PAUSED) {
            osal_printk("[PANEL_CMD] resume rejected: state=%d\r\n", g_model.state);
            break;
        }
        if (panel_rx_commands_request_exec_resume() != ERRCODE_SUCC) {
            osal_printk("[PANEL_CMD] resume queue failed\r\n");
        }
        break;
    case 3:
        if (!perms.can_abort) {
            osal_printk("[PANEL_CMD] force_stop rejected: state=%d\r\n", g_model.state);
            break;
        }
        if (panel_rx_commands_request_abort() == ERRCODE_SUCC) {
            panel_model_request_abort();
        } else {
            osal_printk("[PANEL_CMD] force_stop queue failed\r\n");
        }
        break;
    case 4:
        osal_printk("[HOME] settings click -> settings\r\n");
        ui_manager_switch_page(PAGE_SETTINGS);
        break;
    }
}

static lv_obj_t *create_action_btn(lv_obj_t *parent, const char *text,
                                    lv_color_t bg_color, lv_obj_t **out_label)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 60, 44);
    lv_obj_set_style_bg_color(btn, bg_color, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 1);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_width(lbl, 58);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_center(lbl);

    if (out_label) *out_label = lbl;
    return btn;
}

static void create_action_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 320, 56);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 184);
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
    bind_click(g_btn_start, btn_event_cb, (void *)0);
    bind_click(g_lbl_start, btn_event_cb, (void *)0);

    g_btn_pause = create_action_btn(bar, "暂停", COLOR_LASER_ORANGE, &g_lbl_pause);
    bind_click(g_btn_pause, btn_event_cb, (void *)1);
    bind_click(g_lbl_pause, btn_event_cb, (void *)1);

    g_btn_resume = create_action_btn(bar, "恢复", COLOR_LASER_BLUE, &g_lbl_resume);
    bind_click(g_btn_resume, btn_event_cb, (void *)2);
    bind_click(g_lbl_resume, btn_event_cb, (void *)2);

    g_btn_force_stop = create_action_btn(bar, "强制停止", COLOR_LASER_RED, &g_lbl_force_stop);
    bind_click(g_btn_force_stop, btn_event_cb, (void *)3);
    bind_click(g_lbl_force_stop, btn_event_cb, (void *)3);

    g_btn_settings = create_action_btn(bar, "设置", COLOR_TEXT_MUTED, &g_lbl_settings);
    bind_click(g_btn_settings, btn_event_cb, (void *)4);
    bind_click(g_lbl_settings, btn_event_cb, (void *)4);
}

static void apply_state(const panel_model_t *model)
{
#define g_model (*model)
    panel_button_permissions_t perms;
    panel_model_get_button_permissions(&perms);
    bool start_en = false, pause_en = false, resume_en = false, force_stop_en = false;

    lv_label_set_text(g_lbl_substate, "待机");

    switch (g_model.state) {
    case SYS_STATE_NO_JOB:
        lv_label_set_text(g_lbl_pct, "0%");
        lv_label_set_text(g_lbl_state_badge, panel_model_state_label(g_model.state));
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_BLUE, 0);
        lv_label_set_text(g_lbl_safety_val, "已锁定");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_YELLOW, 0);
        break;

    case SYS_STATE_BROWSING:
        lv_label_set_text(g_lbl_pct, "0%");
        lv_label_set_text(g_lbl_substate, panel_model_state_detail(g_model.state));
        lv_label_set_text(g_lbl_state_badge, panel_model_state_label(g_model.state));
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
        lv_label_set_text(g_lbl_substate, panel_model_state_detail(g_model.state));
        lv_label_set_text(g_lbl_state_badge, panel_model_state_label(g_model.state));
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
        lv_label_set_text(g_lbl_substate, panel_model_state_detail(g_model.state));
        lv_label_set_text(g_lbl_state_badge, panel_model_state_label(g_model.state));
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_BLUE, 0);
        lv_label_set_text(g_lbl_safety_val, "关闭");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_GREEN, 0);
        break;

    case SYS_STATE_READY:
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", g_model.progress);
            lv_label_set_text(g_lbl_pct, buf);
        }
        lv_label_set_text(g_lbl_substate, panel_model_state_detail(g_model.state));
        lv_label_set_text(g_lbl_state_badge, panel_model_state_label(g_model.state));
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
        lv_label_set_text(g_lbl_substate, panel_model_state_detail(g_model.state));
        lv_label_set_text(g_lbl_state_badge, panel_model_state_label(g_model.state));
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_RED, 0);
        lv_label_set_text(g_lbl_safety_val,
            g_model.laser_output_active ? "激光中" : "关闭");
        lv_obj_set_style_text_color(g_lbl_safety_val,
            g_model.laser_output_active ? COLOR_LASER_RED : COLOR_LASER_GREEN, 0);
        break;

    case SYS_STATE_DONE:
        lv_label_set_text(g_lbl_pct, "100%");
        lv_label_set_text(g_lbl_substate, panel_model_state_detail(g_model.state));
        lv_label_set_text(g_lbl_state_badge, panel_model_state_label(g_model.state));
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_GREEN, 0);
        lv_label_set_text(g_lbl_safety_val, "关闭");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_GREEN, 0);
        break;

    case SYS_STATE_PAUSED:
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", g_model.progress);
            lv_label_set_text(g_lbl_pct, buf);
        }
        lv_label_set_text(g_lbl_substate, panel_model_state_detail(g_model.state));
        lv_label_set_text(g_lbl_state_badge, panel_model_state_label(g_model.state));
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_ORANGE, 0);
        lv_label_set_text(g_lbl_safety_val, "关闭");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_GREEN, 0);
        break;

    case SYS_STATE_REQUESTING_STOP:
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", g_model.progress);
            lv_label_set_text(g_lbl_pct, buf);
        }
        lv_label_set_text(g_lbl_substate, panel_model_state_detail(g_model.state));
        lv_label_set_text(g_lbl_state_badge, panel_model_state_label(g_model.state));
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
        lv_label_set_text(g_lbl_substate, panel_model_state_detail(g_model.state));
        lv_label_set_text(g_lbl_state_badge, panel_model_state_label(g_model.state));
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
        lv_label_set_text(g_lbl_substate, panel_model_state_detail(g_model.state));
        lv_label_set_text(g_lbl_state_badge, panel_model_state_label(g_model.state));
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_ORANGE, 0);
        lv_label_set_text(g_lbl_safety_val, "待执行");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_YELLOW, 0);
        break;

    case SYS_STATE_TERMINATED:
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", g_model.progress);
            lv_label_set_text(g_lbl_pct, buf);
        }
        lv_label_set_text(g_lbl_substate, panel_model_state_detail(g_model.state));
        lv_label_set_text(g_lbl_state_badge, panel_model_state_label(g_model.state));
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_RED, 0);
        lv_label_set_text(g_lbl_safety_val, "关闭");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_GREEN, 0);
        break;

    case SYS_STATE_ERROR:
        lv_label_set_text(g_lbl_pct, "0%");
        lv_label_set_text(g_lbl_substate, panel_model_state_detail(g_model.state));
        lv_label_set_text(g_lbl_state_badge, panel_model_state_label(g_model.state));
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_LASER_RED, 0);
        lv_label_set_text(g_lbl_safety_val, "已锁定");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_LASER_RED, 0);
        force_stop_en = true;
        break;

    case SYS_STATE_LINK_LOST:
        lv_label_set_text(g_lbl_pct, "0%");
        lv_label_set_text(g_lbl_substate, panel_model_state_detail(g_model.state));
        lv_label_set_text(g_lbl_state_badge, panel_model_state_label(g_model.state));
        lv_obj_set_style_text_color(g_lbl_state_badge, COLOR_TEXT_MUTED, 0);
        lv_label_set_text(g_lbl_safety_val, "未知");
        lv_obj_set_style_text_color(g_lbl_safety_val, COLOR_TEXT_MUTED, 0);
        break;

    default:
        break;
    }

    start_en = perms.can_start && g_model.state != SYS_STATE_PAUSED;
    if (g_model.view_mode == PANEL_VIEW_OFFLINE && g_model.state == SYS_STATE_READY) {
        start_en = start_en && panel_file_manager_get_selected() != NULL &&
            panel_offline_job_is_ready() && panel_transport_sle_rx_is_connected() &&
            panel_transport_sle_can_control_rx();
    }
    pause_en = perms.can_stop;
    resume_en = perms.can_start && g_model.state == SYS_STATE_PAUSED;
    force_stop_en = perms.can_abort;

    {
        char time_buf[12];
        uint32_t min = g_model.job_seconds / 60;
        uint32_t sec = g_model.job_seconds % 60;
        snprintf(time_buf, sizeof(time_buf), "%02lu:%02lu", (unsigned long)min, (unsigned long)sec);
        lv_label_set_text(g_lbl_job_time, time_buf);
    }
    lv_label_set_text(g_lbl_job_name, g_model.job_name);

    if (g_title_dot != NULL) {
        lv_obj_set_style_bg_color(g_title_dot,
            g_model.view_mode == PANEL_VIEW_OFFLINE ? COLOR_LASER_YELLOW : COLOR_LASER_BLUE, 0);
    }
    if (g_lbl_title != NULL) {
        lv_obj_set_style_text_color(g_lbl_title,
            g_model.view_mode == PANEL_VIEW_OFFLINE ? COLOR_LASER_YELLOW : COLOR_LASER_BLUE, 0);
    }

    bool status_source_connected = (g_model.view_mode == PANEL_VIEW_ONLINE) ?
        g_model.tx_connected : g_model.rx_connected;
    lv_obj_set_style_text_color(g_lbl_rx,
        status_source_connected ? COLOR_LASER_GREEN : COLOR_LASER_RED, 0);
    if (g_model.view_mode == PANEL_VIEW_ONLINE) {
        lv_label_set_text(g_lbl_rx, status_source_connected ? "TX正常" : "TX断开");
    } else {
        lv_label_set_text(g_lbl_rx, status_source_connected ? "RX正常" : "RX断开");
    }
    lv_obj_set_style_text_color(g_lbl_sle,
        g_model.view_mode == PANEL_VIEW_OFFLINE ? COLOR_LASER_ORANGE :
        (g_model.sle_connected ? COLOR_LASER_BLUE : COLOR_TEXT_MUTED), 0);
    lv_label_set_text(g_lbl_sle,
        g_model.view_mode == PANEL_VIEW_OFFLINE ? "离线" : panel_model_mode_label(g_model.mode));

    const panel_file_entry_t *selected = panel_file_manager_get_selected();
    if (selected != NULL) {
        lv_label_set_text(g_lbl_sd_file, selected->name);
    } else {
        lv_label_set_text(g_lbl_sd_file, "点击选择");
    }

    lv_obj_set_style_bg_opa(g_btn_start, start_en ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_text_opa(g_lbl_start, start_en ? LV_OPA_COVER : LV_OPA_50, 0);
    if (start_en) {
        lv_obj_add_flag(g_btn_start, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(g_lbl_start, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(g_btn_start, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(g_lbl_start, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_style_bg_opa(g_btn_pause, pause_en ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_text_opa(g_lbl_pause, pause_en ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_bg_opa(g_btn_resume, resume_en ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_text_opa(g_lbl_resume, resume_en ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_bg_opa(g_btn_force_stop, force_stop_en ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_text_opa(g_lbl_force_stop, force_stop_en ? LV_OPA_COVER : LV_OPA_50, 0);

    lv_label_set_text(g_lbl_start, "启动");
    lv_label_set_text(g_lbl_pause, perms.requesting_stop ? "暂停中" : "暂停");
    lv_label_set_text(g_lbl_resume, "恢复");
    lv_label_set_text(g_lbl_force_stop, perms.requesting_abort ? "停止中" : "强制停止");
#undef g_model
}

void home_page_create(lv_obj_t *parent)
{
    panel_model_t model;
    panel_model_get_snapshot(&model);
    lv_obj_t *scr = parent;
    lv_obj_remove_style_all(scr);
    lv_obj_add_style(scr, &style_screen, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    create_status_bar(scr, &model);

    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_set_size(body, 320, 152);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 32);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(body, 8, 0);
    lv_obj_set_style_pad_left(body, 8, 0);
    lv_obj_set_style_pad_top(body, 8, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);

    create_progress_block(body, &model);
    create_info_block(body);
    create_action_bar(scr);
    g_rendered_model_seq = UINT32_MAX;
    g_rendered_file_seq = UINT32_MAX;
}

void home_page_update(void)
{
    const panel_file_manager_t *file_mgr = panel_file_manager_get();
    uint32_t file_seq = (file_mgr != NULL) ? file_mgr->seq : 0U;
    panel_model_t model;
    panel_model_get_snapshot(&model);
    if (g_rendered_model_seq == model.seq && g_rendered_file_seq == file_seq) {
        return;
    }

    apply_state(&model);
    g_rendered_model_seq = model.seq;
    g_rendered_file_seq = file_seq;
}
