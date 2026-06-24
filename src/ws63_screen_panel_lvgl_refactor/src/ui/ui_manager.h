/**
 * @file ui_manager.h
 * @brief UI manager: page navigation, lazy init, state dispatch.
 */
#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "errcode.h"
#include "lvgl.h"

typedef enum {
    PAGE_HOME = 0,
    PAGE_SETTINGS,
    PAGE_ALERT_SOUND,
    PAGE_DIAGNOSTICS,
    PAGE_JOB_MONITOR,
    PAGE_CONTROL,
    PAGE_COUNT
} page_id_t;

errcode_t ui_manager_init(void);
void ui_manager_update(void);

void ui_manager_switch_page(page_id_t id);
page_id_t ui_manager_get_current(void);

void ui_manager_notify_state_changed(void);

#endif
