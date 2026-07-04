/**
 * @file page_job_monitor.c
 * @brief Job Monitor page: detailed execution data cards.
 */
#include "page_job_monitor.h"
#include "panel_theme.h"
#include "ui_manager.h"
#include "../service/panel_model.h"
#include "soc_osal.h"
#include <stdio.h>

static lv_obj_t *g_lbl_job_id;
static lv_obj_t *g_lbl_title;
static lv_obj_t *g_lbl_owner;
static lv_obj_t *g_lbl_mode;
static lv_obj_t *g_lbl_lines_total;
static lv_obj_t *g_lbl_lines_exec;
static lv_obj_t *g_lbl_bytes_rx;
static lv_obj_t *g_lbl_bytes_total;
static lv_obj_t *g_lbl_cache_free;
static lv_obj_t *g_lbl_pos_x;
static lv_obj_t *g_lbl_pos_y;
static lv_obj_t *g_lbl_feed;
static lv_obj_t *g_lbl_power;
static uint32_t g_rendered_seq = UINT32_MAX;

static void bind_click(lv_obj_t *obj, lv_event_cb_t cb, void *user_data)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(obj, 6);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, user_data);
}

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    osal_printk("[MONITOR] back -> home\r\n");
    ui_manager_switch_page(PAGE_HOME);
}

static lv_obj_t *create_stat_row(lv_obj_t *parent, const char *label,
                                  lv_obj_t **out_val, lv_color_t accent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 130, 18);
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

    *out_val = lv_label_create(row);
    lv_label_set_text(*out_val, "--");
    lv_obj_set_style_text_font(*out_val, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(*out_val, accent, 0);

    return row;
}

void page_job_monitor_create(lv_obj_t *parent)
{
    g_rendered_seq = UINT32_MAX;

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
    bind_click(back_lbl, back_btn_cb, NULL);

    g_lbl_title = lv_label_create(header);
    lv_label_set_text(g_lbl_title, "任务控制");
    lv_obj_set_style_text_font(g_lbl_title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(g_lbl_title, COLOR_TEXT_BRIGHT, 0);

    /* Body */
    lv_obj_t *body = lv_obj_create(scr);
    panel_page_body_setup(body, 8);

    /* Job info card */
    lv_obj_t *job_card = lv_obj_create(body);
    lv_obj_set_size(job_card, 290, 74);
    lv_obj_remove_flag(job_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(job_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(job_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(job_card, 2, 0);
    lv_obj_add_style(job_card, &style_card, 0);

    lv_obj_t *job_title = lv_label_create(job_card);
    lv_label_set_text(job_title, LV_SYMBOL_FILE " 任务信息");
    lv_obj_set_style_text_font(job_title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(job_title, COLOR_LASER_BLUE, 0);

    lv_obj_t *owner_row = lv_obj_create(job_card);
    lv_obj_set_size(owner_row, 270, 18);
    lv_obj_remove_flag(owner_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(owner_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(owner_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(owner_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(owner_row, 0, 0);
    lv_obj_set_style_pad_all(owner_row, 0, 0);

    lv_obj_t *owner_lbl = lv_label_create(owner_row);
    lv_label_set_text(owner_lbl, "控制源");
    lv_obj_set_style_text_font(owner_lbl, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(owner_lbl, COLOR_TEXT_MUTED, 0);

    g_lbl_owner = lv_label_create(owner_row);
    lv_label_set_text(g_lbl_owner, "--");
    lv_obj_set_style_text_font(g_lbl_owner, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(g_lbl_owner, COLOR_TEXT_LIGHT, 0);

    lv_obj_t *mode_lbl = lv_label_create(owner_row);
    lv_label_set_text(mode_lbl, "模式");
    lv_obj_set_style_text_font(mode_lbl, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(mode_lbl, COLOR_TEXT_MUTED, 0);

    g_lbl_mode = lv_label_create(owner_row);
    lv_label_set_text(g_lbl_mode, "--");
    lv_obj_set_style_text_font(g_lbl_mode, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(g_lbl_mode, COLOR_TEXT_LIGHT, 0);

    lv_obj_t *job_row = lv_obj_create(job_card);
    lv_obj_set_size(job_row, 270, 18);
    lv_obj_remove_flag(job_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(job_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(job_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(job_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(job_row, 0, 0);
    lv_obj_set_style_pad_all(job_row, 0, 0);

    lv_obj_t *id_lbl = lv_label_create(job_row);
    lv_label_set_text(id_lbl, "任务名");
    lv_obj_set_style_text_font(id_lbl, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(id_lbl, COLOR_TEXT_MUTED, 0);

    g_lbl_job_id = lv_label_create(job_row);
    lv_label_set_text(g_lbl_job_id, "--");
    lv_obj_set_style_text_font(g_lbl_job_id, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(g_lbl_job_id, COLOR_TEXT_LIGHT, 0);

    /* Progress card */
    lv_obj_t *prog_card = lv_obj_create(body);
    lv_obj_set_size(prog_card, 290, 64);
    lv_obj_remove_flag(prog_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(prog_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(prog_card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_style(prog_card, &style_card, 0);

    /* Left column: lines */
    lv_obj_t *col_left = lv_obj_create(prog_card);
    lv_obj_set_size(col_left, 130, 50);
    lv_obj_remove_flag(col_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col_left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(col_left, 2, 0);
    lv_obj_set_style_bg_opa(col_left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col_left, 0, 0);
    lv_obj_set_style_pad_all(col_left, 0, 0);

    create_stat_row(col_left, "已执行行", &g_lbl_lines_exec, COLOR_LASER_GREEN);
    create_stat_row(col_left, "总行数", &g_lbl_lines_total, COLOR_TEXT_LIGHT);

    /* Right column: bytes */
    lv_obj_t *col_right = lv_obj_create(prog_card);
    lv_obj_set_size(col_right, 130, 50);
    lv_obj_remove_flag(col_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col_right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col_right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(col_right, 2, 0);
    lv_obj_set_style_bg_opa(col_right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col_right, 0, 0);
    lv_obj_set_style_pad_all(col_right, 0, 0);

    create_stat_row(col_right, "已接收", &g_lbl_bytes_rx, COLOR_LASER_BLUE);
    create_stat_row(col_right, "总字节", &g_lbl_bytes_total, COLOR_TEXT_LIGHT);

    /* Motion card */
    lv_obj_t *motion_card = lv_obj_create(body);
    lv_obj_set_size(motion_card, 290, 64);
    lv_obj_remove_flag(motion_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(motion_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(motion_card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_style(motion_card, &style_card, 0);

    lv_obj_t *mcol_l = lv_obj_create(motion_card);
    lv_obj_set_size(mcol_l, 130, 50);
    lv_obj_remove_flag(mcol_l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(mcol_l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mcol_l, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(mcol_l, 2, 0);
    lv_obj_set_style_bg_opa(mcol_l, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mcol_l, 0, 0);
    lv_obj_set_style_pad_all(mcol_l, 0, 0);

    create_stat_row(mcol_l, "X坐标", &g_lbl_pos_x, COLOR_TEXT_LIGHT);
    create_stat_row(mcol_l, "Y坐标", &g_lbl_pos_y, COLOR_TEXT_LIGHT);

    lv_obj_t *mcol_r = lv_obj_create(motion_card);
    lv_obj_set_size(mcol_r, 130, 50);
    lv_obj_remove_flag(mcol_r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(mcol_r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mcol_r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(mcol_r, 2, 0);
    lv_obj_set_style_bg_opa(mcol_r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mcol_r, 0, 0);
    lv_obj_set_style_pad_all(mcol_r, 0, 0);

    create_stat_row(mcol_r, "进给速度", &g_lbl_feed, COLOR_LASER_ORANGE);
    create_stat_row(mcol_r, "激光功率", &g_lbl_power, COLOR_LASER_RED);

    /* Cache card */
    lv_obj_t *cache_card = lv_obj_create(body);
    lv_obj_set_size(cache_card, 290, 28);
    lv_obj_remove_flag(cache_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cache_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cache_card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_style(cache_card, &style_card, 0);

    lv_obj_t *cache_lbl = lv_label_create(cache_card);
    lv_label_set_text(cache_lbl, "缓存剩余");
    lv_obj_set_style_text_font(cache_lbl, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(cache_lbl, COLOR_TEXT_MUTED, 0);

    g_lbl_cache_free = lv_label_create(cache_card);
    lv_label_set_text(g_lbl_cache_free, "128 KB");
    lv_obj_set_style_text_font(g_lbl_cache_free, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(g_lbl_cache_free, COLOR_LASER_GREEN, 0);
}

void page_job_monitor_update(void)
{
    /* Update from the RX-backed panel model. */
    char buf[24];

    if (g_rendered_seq == g_model.seq) {
        return;
    }

    if (g_model.view_mode == PANEL_VIEW_OFFLINE) {
        lv_label_set_text(g_lbl_title, "离线任务");
    } else if (g_model.owner == PANEL_OWNER_HOST) {
        lv_label_set_text(g_lbl_title, "在线监控");
    } else if (g_model.owner == PANEL_OWNER_SCREEN) {
        lv_label_set_text(g_lbl_title, "离线监控");
    } else {
        lv_label_set_text(g_lbl_title, "任务控制");
    }

    lv_label_set_text(g_lbl_owner, panel_model_owner_label(g_model.owner));
    lv_label_set_text(g_lbl_mode, panel_model_mode_label(g_model.mode));
    lv_label_set_text(g_lbl_job_id, g_model.job_name);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)g_model.executed_lines);
    lv_label_set_text(g_lbl_lines_exec, buf);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)g_model.total_lines);
    lv_label_set_text(g_lbl_lines_total, buf);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)g_model.received_size);
    lv_label_set_text(g_lbl_bytes_rx, buf);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)g_model.total_size);
    lv_label_set_text(g_lbl_bytes_total, buf);

    snprintf(buf, sizeof(buf), "%.1f", g_model.progress * 0.7f);
    lv_label_set_text(g_lbl_pos_x, buf);
    snprintf(buf, sizeof(buf), "%.1f", g_model.progress * 0.5f);
    lv_label_set_text(g_lbl_pos_y, buf);

    lv_label_set_text(g_lbl_feed, "1000");
    lv_label_set_text(g_lbl_power, g_model.laser_output_active ? "S500" : "S0");

    snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)(g_model.cache_free / 1024U));
    lv_label_set_text(g_lbl_cache_free, buf);
    g_rendered_seq = g_model.seq;
}
