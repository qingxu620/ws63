/**
 * @file ui_manager.c
 * @brief UI manager: page routing with lazy init, 100ms fade transitions.
 */
#include "ui_manager.h"
#include "panel_theme.h"
#include "pages/home_page.h"
#include "pages/page_settings.h"
#include "pages/page_alert_sound.h"
#include "pages/page_diagnostics.h"
#include "pages/page_job_monitor.h"
#include "pages/page_control.h"
#include "soc_osal.h"

static page_id_t g_current_page = PAGE_HOME;

typedef struct {
    lv_obj_t *screen;
    bool created;
    void (*create_fn)(lv_obj_t *);
    void (*update_fn)(void);
} page_entry_t;

static page_entry_t g_pages[PAGE_COUNT];

static void register_page(page_id_t id, void (*create)(lv_obj_t *), void (*update)(void))
{
    g_pages[id].screen = NULL;
    g_pages[id].created = false;
    g_pages[id].create_fn = create;
    g_pages[id].update_fn = update;
}

errcode_t ui_manager_init(void)
{
    panel_theme_init();

    memset(g_pages, 0, sizeof(g_pages));
    register_page(PAGE_HOME, home_page_create, home_page_update);
    register_page(PAGE_SETTINGS, page_settings_create, page_settings_update);
    register_page(PAGE_ALERT_SOUND, page_alert_sound_create, page_alert_sound_update);
    register_page(PAGE_DIAGNOSTICS, page_diagnostics_create, page_diagnostics_update);
    register_page(PAGE_JOB_MONITOR, page_job_monitor_create, page_job_monitor_update);
    register_page(PAGE_CONTROL, page_control_create, page_control_update);

    g_pages[PAGE_HOME].screen = lv_scr_act();
    g_pages[PAGE_HOME].create_fn(g_pages[PAGE_HOME].screen);
    g_pages[PAGE_HOME].created = true;
    g_current_page = PAGE_HOME;

    osal_printk("[UI] manager init, dashboard created\r\n");
    return ERRCODE_SUCC;
}

void ui_manager_switch_page(page_id_t id)
{
    if (id >= PAGE_COUNT || id == g_current_page) return;

    page_entry_t *entry = &g_pages[id];
    if (!entry->created) {
        entry->screen = lv_obj_create(NULL);
        entry->create_fn(entry->screen);
        entry->created = true;
    }

    lv_scr_load_anim(entry->screen, LV_SCR_LOAD_ANIM_FADE_ON, 100, 0, false);
    g_current_page = id;
}

page_id_t ui_manager_get_current(void)
{
    return g_current_page;
}

void ui_manager_update(void)
{
    if (g_current_page < PAGE_COUNT && g_pages[g_current_page].update_fn) {
        g_pages[g_current_page].update_fn();
    }
}

void ui_manager_notify_state_changed(void)
{
    if (g_current_page == PAGE_HOME) {
        home_page_update();
    }
}
