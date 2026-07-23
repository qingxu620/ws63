/**
 * @file page_settings.c
 * @brief Settings page: brightness slider + about info + navigation.
 */
#include "page_settings.h"
#include "panel_theme.h"
#include "ui_manager.h"
#include "lcd_driver.h"
#include "../service/panel_model.h"
#include "soc_osal.h"
#include <stdio.h>

static lv_obj_t *g_slider_brightness;
static lv_obj_t *g_lbl_brightness_val;
static uint8_t g_pending_brightness = 80U;
static uint8_t g_applied_brightness = 80U;

static void bind_click(lv_obj_t *obj, lv_event_cb_t cb, void *user_data)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(obj, 6);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, user_data);
}

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    osal_printk("[SETTINGS] back -> home\r\n");
    ui_manager_switch_page(PAGE_HOME);
}

static void apply_brightness(uint8_t val)
{
    if (val == g_applied_brightness) {
        return;
    }

    errcode_t ret = lcd_set_brightness(val);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[SETTINGS] brightness=%u PWM5 update failed: 0x%x\r\n",
                    (unsigned int)val, ret);
        return;
    }
    g_applied_brightness = val;
}

static void brightness_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    int val = lv_slider_get_value(slider);

    if (val < 10) {
        val = 10;
    } else if (val > 100) {
        val = 100;
    }

    g_pending_brightness = (uint8_t)val;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", val);
    lv_label_set_text(g_lbl_brightness_val, buf);

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        apply_brightness(g_pending_brightness);
    }
}

static void file_btn_cb(lv_event_t *e)
{
    (void)e;
    panel_model_t model;
    panel_model_get_snapshot(&model);
    if (model.view_mode == PANEL_VIEW_ONLINE) {
        panel_model_toggle_primary_mode();
        panel_model_get_snapshot(&model);
        if (model.view_mode != PANEL_VIEW_OFFLINE) {
            osal_printk("[SETTINGS] SD task rejected: online job still active\r\n");
            return;
        }
    }
    osal_printk("[SETTINGS] file -> offline file browser\r\n");
    ui_manager_switch_page(PAGE_FILE_BROWSER);
}

static void monitor_btn_cb(lv_event_t *e)
{
    (void)e;
    osal_printk("[SETTINGS] monitor -> job monitor\r\n");
    ui_manager_switch_page(PAGE_JOB_MONITOR);
}

static void diagnostics_btn_cb(lv_event_t *e)
{
    (void)e;
    osal_printk("[SETTINGS] diagnostics -> diagnostics\r\n");
    ui_manager_switch_page(PAGE_DIAGNOSTICS);
}

static lv_obj_t *create_nav_btn(lv_obj_t *parent, const char *symbol,
                                 const char *label, lv_color_t accent,
                                 lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 290, 36);
    lv_obj_set_style_bg_color(btn, COLOR_BG_CARD, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_color(btn, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);

    lv_obj_t *row = lv_obj_create(btn);
    lv_obj_set_size(row, 270, 24);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    bind_click(row, cb, NULL);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text_fmt(lbl, "%s %s", symbol, label);
    lv_obj_set_style_text_font(lbl, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(lbl, accent, 0);
    bind_click(lbl, cb, NULL);

    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, COLOR_TEXT_MUTED, 0);
    bind_click(arrow, cb, NULL);

    bind_click(btn, cb, NULL);
    return btn;
}

void page_settings_create(lv_obj_t *parent)
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
    bind_click(back_lbl, back_btn_cb, NULL);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "系统设置");
    lv_obj_set_style_text_font(title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(title, COLOR_TEXT_BRIGHT, 0);

    /* Body */
    lv_obj_t *body = lv_obj_create(scr);
    panel_page_body_setup(body, 12);

    /* Brightness card with horizontal slider */
    lv_obj_t *bright_card = lv_obj_create(body);
    lv_obj_set_size(bright_card, 290, 92);
    lv_obj_remove_flag(bright_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bright_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bright_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(bright_card, 10, 0);
    lv_obj_add_style(bright_card, &style_card, 0);

    lv_obj_t *top_row = lv_obj_create(bright_card);
    lv_obj_set_size(top_row, 266, 24);
    lv_obj_remove_flag(top_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(top_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_row, 0, 0);
    lv_obj_set_style_pad_all(top_row, 0, 0);

    lv_obj_t *bright_lbl = lv_label_create(top_row);
    lv_label_set_text(bright_lbl, LV_SYMBOL_SETTINGS " 屏幕亮度");
    lv_obj_set_style_text_font(bright_lbl, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(bright_lbl, COLOR_LASER_ORANGE, 0);

    g_lbl_brightness_val = lv_label_create(top_row);
    lv_label_set_text(g_lbl_brightness_val, "80%");
    lv_obj_set_style_text_font(g_lbl_brightness_val, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_lbl_brightness_val, COLOR_TEXT_BRIGHT, 0);

    g_slider_brightness = lv_slider_create(bright_card);
    lv_obj_set_size(g_slider_brightness, 266, 24);
    lv_obj_set_ext_click_area(g_slider_brightness, 8);
    lv_slider_set_range(g_slider_brightness, 10, 100);
    lv_slider_set_value(g_slider_brightness, 80, LV_ANIM_OFF);
    lv_obj_set_style_radius(g_slider_brightness, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_slider_brightness, COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_slider_brightness, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_slider_brightness, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_slider_brightness, COLOR_LASER_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_slider_brightness, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_slider_brightness, COLOR_TEXT_BRIGHT, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(g_slider_brightness, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(g_slider_brightness, 10, LV_PART_KNOB);
    lv_obj_add_event_cb(g_slider_brightness, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(g_slider_brightness, brightness_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(g_slider_brightness, brightness_cb, LV_EVENT_PRESS_LOST, NULL);

    create_nav_btn(body, LV_SYMBOL_PLAY, "任务控制", COLOR_LASER_GREEN, monitor_btn_cb);
    create_nav_btn(body, LV_SYMBOL_DIRECTORY, "SD任务", COLOR_LASER_BLUE, file_btn_cb);
    create_nav_btn(body, LV_SYMBOL_WARNING, "诊断日志", COLOR_LASER_ORANGE, diagnostics_btn_cb);

    /* About card */
    lv_obj_t *about_card = lv_obj_create(body);
    lv_obj_set_size(about_card, 290, 60);
    lv_obj_remove_flag(about_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(about_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(about_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(about_card, 2, 0);
    lv_obj_add_style(about_card, &style_card, 0);

    lv_obj_t *about_title = lv_label_create(about_card);
    lv_label_set_text(about_title, LV_SYMBOL_WARNING " 运行基线");
    lv_obj_set_style_text_font(about_title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(about_title, COLOR_LASER_ORANGE, 0);

    lv_obj_t *fw_ver = lv_label_create(about_card);
    lv_label_set_text(fw_ver, "模式：SLE / RX真相源");
    lv_obj_set_style_text_font(fw_ver, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(fw_ver, COLOR_TEXT_LIGHT, 0);

    lv_obj_t *hw_info = lv_label_create(about_card);
    lv_label_set_text(hw_info, "安全：STOP/ABORT/FOCUS_OFF");
    lv_obj_set_style_text_font(hw_info, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(hw_info, COLOR_TEXT_MUTED, 0);
}

void page_settings_update(void)
{
}
