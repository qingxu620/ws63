/**
 * @file page_diagnostics.c
 * @brief Diagnostics page: SLE stats, memory, connection info.
 */
#include "page_diagnostics.h"
#include "panel_theme.h"
#include "ui_manager.h"
#include "soc_osal.h"
#include <stdio.h>

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_switch_page(PAGE_HOME);
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

    /* Body */
    lv_obj_t *body = lv_obj_create(scr);
    panel_page_body_setup(body, 6);

    /* Connection section */
    lv_obj_t *conn_card = lv_obj_create(body);
    lv_obj_set_size(conn_card, 290, 90);
    lv_obj_remove_flag(conn_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(conn_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(conn_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(conn_card, 2, 0);
    lv_obj_add_style(conn_card, &style_card, 0);

    lv_obj_t *conn_title = lv_label_create(conn_card);
    lv_label_set_text(conn_title, LV_SYMBOL_WIFI " 连接状态");
    lv_obj_set_style_text_font(conn_title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(conn_title, COLOR_LASER_BLUE, 0);

    create_info_row(conn_card, "SLE状态", "已连接", COLOR_LASER_GREEN);
    create_info_row(conn_card, "RX链路", "正常", COLOR_LASER_GREEN);
    create_info_row(conn_card, "RSSI", "-42 dBm", COLOR_TEXT_LIGHT);

    /* SLE Stats section */
    lv_obj_t *sle_card = lv_obj_create(body);
    lv_obj_set_size(sle_card, 290, 90);
    lv_obj_remove_flag(sle_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sle_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sle_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(sle_card, 2, 0);
    lv_obj_add_style(sle_card, &style_card, 0);

    lv_obj_t *sle_title = lv_label_create(sle_card);
    lv_label_set_text(sle_title, LV_SYMBOL_CHARGE " SLE统计");
    lv_obj_set_style_text_font(sle_title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(sle_title, COLOR_LASER_ORANGE, 0);

    create_info_row(sle_card, "TX发送包", "0", COLOR_TEXT_LIGHT);
    create_info_row(sle_card, "RX接收包", "0", COLOR_TEXT_LIGHT);
    create_info_row(sle_card, "ACK/NACK", "0 / 0", COLOR_TEXT_LIGHT);

    /* System section */
    lv_obj_t *sys_card = lv_obj_create(body);
    lv_obj_set_size(sys_card, 290, 70);
    lv_obj_remove_flag(sys_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sys_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sys_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(sys_card, 2, 0);
    lv_obj_add_style(sys_card, &style_card, 0);

    lv_obj_t *sys_title = lv_label_create(sys_card);
    lv_label_set_text(sys_title, LV_SYMBOL_HOME " 系统信息");
    lv_obj_set_style_text_font(sys_title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(sys_title, COLOR_LASER_YELLOW, 0);

    create_info_row(sys_card, "LVGL堆内存", "48 KB", COLOR_TEXT_LIGHT);
    create_info_row(sys_card, "运行时间", "00:00", COLOR_TEXT_LIGHT);
}

void page_diagnostics_update(void)
{
    /* Static demo data, no periodic updates */
}
