/**
 * @file lv_demo_panel.c
 * @brief LVGL demo page: WS63 Laser Panel.
 *
 * Layout (all fake data, no SLE/RX/Host):
 *   Title:   WS63 Laser Panel
 *   Status:  RX: DISCONNECTED
 *   Task:    No Job
 *   Bar:     0%
 *   Buttons: [Start] [Pause] [Stop]
 *   Touch:   Touch: x=--- y=---
 */
#include "lv_demo_panel.h"
#include "lvgl.h"
#include "soc_osal.h"
#include <stdio.h>

static lv_obj_t *g_lbl_status;
static lv_obj_t *g_lbl_task;
static lv_obj_t *g_bar;
static lv_obj_t *g_lbl_touch;

static volatile bool g_touch_pressed = false;
static volatile int16_t g_touch_x = 0;
static volatile int16_t g_touch_y = 0;

void lv_demo_panel_update_touch(bool pressed, int16_t x, int16_t y)
{
    g_touch_pressed = pressed;
    g_touch_x = x;
    g_touch_y = y;
}

static void btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) {
        return;
    }

    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    const char *text = lv_label_get_text(label);

    if (text[2] == 'a') {
        /* Start */
        lv_label_set_text(g_lbl_status, "RX: CONNECTED");
        lv_label_set_text(g_lbl_task, "Job: engrave_test.gcode");
        lv_bar_set_value(g_bar, 0, LV_ANIM_OFF);
    } else if (text[0] == 'P') {
        /* Pause */
        lv_label_set_text(g_lbl_status, "RX: PAUSED");
    } else if (text[2] == 'o') {
        /* Stop */
        lv_label_set_text(g_lbl_status, "RX: DISCONNECTED");
        lv_label_set_text(g_lbl_task, "No Job");
        lv_bar_set_value(g_bar, 0, LV_ANIM_OFF);
    }
}

static void update_touch_cb(lv_timer_t *timer)
{
    (void)timer;
    char buf[32];
    if (g_touch_pressed) {
        snprintf(buf, sizeof(buf), "Touch: x=%3d y=%3d", g_touch_x, g_touch_y);
    } else {
        snprintf(buf, sizeof(buf), "Touch: x=--- y=---");
    }
    lv_label_set_text(g_lbl_touch, buf);
}

void lv_demo_panel_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scr, 8, 0);
    lv_obj_set_style_pad_gap(scr, 6, 0);

    /* Title */
    lv_obj_t *lbl_title = lv_label_create(scr);
    lv_label_set_text(lbl_title, "WS63 Laser Panel");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_22, 0);

    /* Status */
    g_lbl_status = lv_label_create(scr);
    lv_label_set_text(g_lbl_status, "RX: DISCONNECTED");

    /* Task */
    g_lbl_task = lv_label_create(scr);
    lv_label_set_text(g_lbl_task, "No Job");

    /* Progress bar */
    g_bar = lv_bar_create(scr);
    lv_obj_set_width(g_bar, lv_pct(90));
    lv_bar_set_range(g_bar, 0, 100);
    lv_bar_set_value(g_bar, 0, LV_ANIM_OFF);

    /* Button row */
    lv_obj_t *btn_row = lv_obj_create(scr);
    lv_obj_set_size(btn_row, lv_pct(90), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(btn_row, 4, 0);
    lv_obj_set_style_pad_gap(btn_row, 8, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);

    static const char *btn_names[] = {"Start", "Pause", "Stop"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_button_create(btn_row);
        lv_obj_set_size(btn, 80, 36);
        lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, btn_names[i]);
        lv_obj_center(lbl);
    }

    /* Touch coordinate label */
    g_lbl_touch = lv_label_create(scr);
    lv_label_set_text(g_lbl_touch, "Touch: x=--- y=---");

    /* Timer to update touch coords */
    lv_timer_create(update_touch_cb, 50, NULL);

    osal_printk("[LVGL] demo page created\r\n");
}
