/**
 * @file page_diagnostics.c
 * @brief Diagnostics page: SLE stats, memory, connection info.
 */
#include "page_diagnostics.h"
#include "panel_theme.h"
#include "ui_manager.h"
#include "../service/panel_model.h"
#include "soc_osal.h"
#include <stdio.h>

static lv_obj_t *g_lbl_host;
static lv_obj_t *g_lbl_tx;
static lv_obj_t *g_lbl_rx;
static lv_obj_t *g_lbl_sle;
static lv_obj_t *g_lbl_owner;
static lv_obj_t *g_lbl_mode;
static lv_obj_t *g_lbl_job_state;
static lv_obj_t *g_lbl_job_id;
static lv_obj_t *g_lbl_progress;
static lv_obj_t *g_lbl_bytes;
static lv_obj_t *g_lbl_lines;
static lv_obj_t *g_lbl_cache;
static lv_obj_t *g_lbl_focus;
static lv_obj_t *g_lbl_laser;
static lv_obj_t *g_lbl_last_error;
static lv_obj_t *g_lbl_seq;

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_switch_page(PAGE_HOME);
}

static void next_scene_cb(lv_event_t *e)
{
    (void)e;
    panel_model_next_scene();
    osal_printk("[DIAG] manual next scene=%s\r\n",
                panel_model_scene_text(g_model.scene));
}

static void mode_toggle_cb(lv_event_t *e)
{
    (void)e;
    panel_model_toggle_primary_mode();
    osal_printk("[DIAG] mode toggle scene=%s owner=%s mode=%s\r\n",
                panel_model_scene_text(g_model.scene),
                panel_model_owner_text(g_model.owner),
                panel_model_mode_text(g_model.mode));
}

static lv_obj_t *create_info_row(lv_obj_t *parent, const char *label, const char *value,
                                  lv_color_t val_color)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 270, 18);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(lbl, COLOR_TEXT_MUTED, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, value);
    lv_obj_set_style_text_font(val, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(val, val_color, 0);

    return val;
}

void page_diagnostics_create(lv_obj_t *parent)
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
    lv_label_set_text(title, "系统诊断");
    lv_obj_set_style_text_font(title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(title, COLOR_TEXT_BRIGHT, 0);

    lv_obj_t *spacer = lv_obj_create(header);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);

    lv_obj_t *mode_btn = lv_button_create(header);
    lv_obj_remove_style_all(mode_btn);
    lv_obj_set_size(mode_btn, 54, 28);
    lv_obj_set_style_bg_color(mode_btn, COLOR_BG_CARD, 0);
    lv_obj_set_style_bg_opa(mode_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mode_btn, 8, 0);
    lv_obj_set_style_border_color(mode_btn, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(mode_btn, 1, 0);
    lv_obj_set_ext_click_area(mode_btn, 6);
    lv_obj_add_event_cb(mode_btn, mode_toggle_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *mode_lbl = lv_label_create(mode_btn);
    lv_label_set_text(mode_lbl, "Mode");
    lv_obj_set_style_text_font(mode_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(mode_lbl, COLOR_LASER_GREEN, 0);
    lv_obj_center(mode_lbl);

    lv_obj_t *next_btn = lv_button_create(header);
    lv_obj_remove_style_all(next_btn);
    lv_obj_set_size(next_btn, 54, 28);
    lv_obj_set_style_bg_color(next_btn, COLOR_BG_CARD, 0);
    lv_obj_set_style_bg_opa(next_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(next_btn, 8, 0);
    lv_obj_set_style_border_color(next_btn, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(next_btn, 1, 0);
    lv_obj_set_ext_click_area(next_btn, 6);
    lv_obj_add_event_cb(next_btn, next_scene_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *next_lbl = lv_label_create(next_btn);
    lv_label_set_text(next_lbl, "Next");
    lv_obj_set_style_text_font(next_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(next_lbl, COLOR_LASER_BLUE, 0);
    lv_obj_center(next_lbl);

    /* Body */
    lv_obj_t *body = lv_obj_create(scr);
    panel_page_body_setup(body, 6);

    /* Connection section */
    lv_obj_t *conn_card = lv_obj_create(body);
    lv_obj_set_size(conn_card, 290, 114);
    lv_obj_remove_flag(conn_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(conn_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(conn_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(conn_card, 2, 0);
    lv_obj_add_style(conn_card, &style_card, 0);

    lv_obj_t *conn_title = lv_label_create(conn_card);
    lv_label_set_text(conn_title, "连接状态");
    lv_obj_set_style_text_font(conn_title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(conn_title, COLOR_LASER_BLUE, 0);

    g_lbl_host = create_info_row(conn_card, "Host", "--", COLOR_TEXT_LIGHT);
    g_lbl_tx = create_info_row(conn_card, "TX链路", "--", COLOR_TEXT_LIGHT);
    g_lbl_rx = create_info_row(conn_card, "RX链路", "--", COLOR_TEXT_LIGHT);
    g_lbl_sle = create_info_row(conn_card, "SLE状态", "--", COLOR_TEXT_LIGHT);

    /* Owner and job section */
    lv_obj_t *sle_card = lv_obj_create(body);
    lv_obj_set_size(sle_card, 290, 162);
    lv_obj_remove_flag(sle_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sle_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sle_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(sle_card, 2, 0);
    lv_obj_add_style(sle_card, &style_card, 0);

    lv_obj_t *sle_title = lv_label_create(sle_card);
    lv_label_set_text(sle_title, "Task Owner");
    lv_obj_set_style_text_font(sle_title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(sle_title, COLOR_LASER_ORANGE, 0);

    g_lbl_owner = create_info_row(sle_card, "Owner", "--", COLOR_TEXT_LIGHT);
    g_lbl_mode = create_info_row(sle_card, "Mode", "--", COLOR_TEXT_LIGHT);
    g_lbl_job_state = create_info_row(sle_card, "Job状态", "--", COLOR_TEXT_LIGHT);
    g_lbl_job_id = create_info_row(sle_card, "Job ID", "--", COLOR_TEXT_LIGHT);
    g_lbl_progress = create_info_row(sle_card, "进度", "--", COLOR_TEXT_LIGHT);
    g_lbl_bytes = create_info_row(sle_card, "接收字节", "--", COLOR_TEXT_LIGHT);
    g_lbl_lines = create_info_row(sle_card, "执行行数", "--", COLOR_TEXT_LIGHT);

    /* RX truth section */
    lv_obj_t *sys_card = lv_obj_create(body);
    lv_obj_set_size(sys_card, 290, 126);
    lv_obj_remove_flag(sys_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sys_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sys_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(sys_card, 2, 0);
    lv_obj_add_style(sys_card, &style_card, 0);

    lv_obj_t *sys_title = lv_label_create(sys_card);
    lv_label_set_text(sys_title, "RX Status");
    lv_obj_set_style_text_font(sys_title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(sys_title, COLOR_LASER_YELLOW, 0);

    g_lbl_cache = create_info_row(sys_card, "Cache剩余", "--", COLOR_TEXT_LIGHT);
    g_lbl_focus = create_info_row(sys_card, "Focus", "--", COLOR_TEXT_LIGHT);
    g_lbl_laser = create_info_row(sys_card, "Laser", "--", COLOR_TEXT_LIGHT);
    g_lbl_last_error = create_info_row(sys_card, "Last error", "--", COLOR_TEXT_LIGHT);
    g_lbl_seq = create_info_row(sys_card, "PANEL seq", "--", COLOR_TEXT_LIGHT);
}

void page_diagnostics_update(void)
{
    char buf[32];

    lv_label_set_text(g_lbl_host, g_model.host_connected ? "已连接" : "未连接");
    lv_obj_set_style_text_color(g_lbl_host,
        g_model.host_connected ? COLOR_LASER_GREEN : COLOR_TEXT_MUTED, 0);
    lv_label_set_text(g_lbl_tx, g_model.tx_connected ? "正常" : "断开");
    lv_obj_set_style_text_color(g_lbl_tx,
        g_model.tx_connected ? COLOR_LASER_GREEN : COLOR_LASER_RED, 0);
    lv_label_set_text(g_lbl_rx, g_model.rx_connected ? "正常" : "断开");
    lv_obj_set_style_text_color(g_lbl_rx,
        g_model.rx_connected ? COLOR_LASER_GREEN : COLOR_LASER_RED, 0);
    lv_label_set_text(g_lbl_sle, g_model.sle_connected ? "已连接" : "未连接");
    lv_obj_set_style_text_color(g_lbl_sle,
        g_model.sle_connected ? COLOR_LASER_GREEN : COLOR_TEXT_MUTED, 0);

    lv_label_set_text(g_lbl_owner, panel_model_owner_text(g_model.owner));
    lv_label_set_text(g_lbl_mode, panel_model_mode_text(g_model.mode));
    lv_label_set_text(g_lbl_job_state, panel_model_state_text(g_model.state));
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)g_model.job_id);
    lv_label_set_text(g_lbl_job_id, buf);
    snprintf(buf, sizeof(buf), "%d%%", g_model.progress);
    lv_label_set_text(g_lbl_progress, buf);
    snprintf(buf, sizeof(buf), "%lu/%lu",
             (unsigned long)g_model.received_size,
             (unsigned long)g_model.total_size);
    lv_label_set_text(g_lbl_bytes, buf);
    snprintf(buf, sizeof(buf), "%lu/%lu",
             (unsigned long)g_model.executed_lines,
             (unsigned long)g_model.total_lines);
    lv_label_set_text(g_lbl_lines, buf);

    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(g_model.cache_free / 1024U));
    lv_label_set_text(g_lbl_cache, buf);
    lv_label_set_text(g_lbl_focus, g_model.focus_active ? "ON" : "OFF");
    lv_obj_set_style_text_color(g_lbl_focus,
        g_model.focus_active ? COLOR_LASER_RED : COLOR_LASER_GREEN, 0);
    lv_label_set_text(g_lbl_laser, g_model.laser_output_active ? "ON" : "OFF");
    lv_obj_set_style_text_color(g_lbl_laser,
        g_model.laser_output_active ? COLOR_LASER_RED : COLOR_LASER_GREEN, 0);
    lv_label_set_text(g_lbl_last_error, g_model.last_error);
    lv_obj_set_style_text_color(g_lbl_last_error,
        g_model.state == SYS_STATE_ERROR || g_model.state == SYS_STATE_LINK_LOST ?
        COLOR_LASER_RED : COLOR_TEXT_LIGHT, 0);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)g_model.seq);
    lv_label_set_text(g_lbl_seq, buf);
}
