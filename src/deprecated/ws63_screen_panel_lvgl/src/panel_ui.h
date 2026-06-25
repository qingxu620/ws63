/**
 * @file panel_ui.h
 * @brief Industrial panel UI: layout, state machine, event handling.
 */
#ifndef PANEL_UI_H
#define PANEL_UI_H

#include "lvgl.h"

typedef enum {
    SYS_STATE_NO_JOB = 0,
    SYS_STATE_RECEIVING,
    SYS_STATE_READY,
    SYS_STATE_RUNNING,
    SYS_STATE_PAUSED,
    SYS_STATE_DONE,
    SYS_STATE_ERROR,
    SYS_STATE_LINK_LOST,
    SYS_STATE_COUNT
} system_state_t;

void panel_ui_create(void);
void panel_ui_set_state(system_state_t state);
void panel_ui_set_progress(int pct);
void panel_ui_set_job_name(const char *name);
void panel_ui_set_job_time(uint32_t seconds);
void panel_ui_set_safety(const char *text, lv_color_t color);
void panel_ui_set_speed(const char *text);
void panel_ui_set_power(const char *text);
void panel_ui_set_rx_status(bool connected);
void panel_ui_set_sle_status(bool connected);
void panel_ui_set_host_status(bool connected);
void panel_ui_update_touch(bool pressed, int16_t x, int16_t y);
system_state_t panel_ui_get_state(void);

#endif
