/**
 * @file panel_model.h
 * @brief Local state model for the panel UI (demo mode).
 */
#ifndef PANEL_MODEL_H
#define PANEL_MODEL_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SYS_STATE_NO_JOB = 0,
    SYS_STATE_BROWSING,
    SYS_STATE_RECEIVING,
    SYS_STATE_SENDING,
    SYS_STATE_READY,
    SYS_STATE_RUNNING,
    SYS_STATE_DONE,
    SYS_STATE_REQUESTING_STOP,
    SYS_STATE_REQUESTING_ABORT,
    SYS_STATE_REQUESTING_FOCUS_OFF,
    SYS_STATE_ERROR,
    SYS_STATE_LINK_LOST,
    SYS_STATE_COUNT
} system_state_t;

typedef enum {
    PANEL_OWNER_NONE = 0,
    PANEL_OWNER_HOST,
    PANEL_OWNER_SCREEN,
    PANEL_OWNER_COUNT
} panel_owner_t;

typedef enum {
    PANEL_MODE_IDLE = 0,
    PANEL_MODE_ONLINE,
    PANEL_MODE_OFFLINE,
    PANEL_MODE_ERROR,
    PANEL_MODE_LINK_LOST,
    PANEL_MODE_COUNT
} panel_mode_t;

typedef enum {
    PANEL_SCENE_IDLE_NONE = 0,
    PANEL_SCENE_HOST_RECEIVING,
    PANEL_SCENE_HOST_READY,
    PANEL_SCENE_HOST_RUNNING,
    PANEL_SCENE_HOST_DONE,
    PANEL_SCENE_HOST_ERROR,
    PANEL_SCENE_HOST_LINK_LOST,
    PANEL_SCENE_SCREEN_BROWSING,
    PANEL_SCENE_SCREEN_SENDING,
    PANEL_SCENE_SCREEN_RUNNING,
    PANEL_SCENE_SCREEN_DONE,
    PANEL_SCENE_REQUESTING_STOP,
    PANEL_SCENE_REQUESTING_ABORT,
    PANEL_SCENE_REQUESTING_FOCUS_OFF,
    PANEL_SCENE_COUNT
} panel_fake_scene_t;

typedef struct {
    bool can_start;
    bool can_stop;
    bool can_abort;
    bool can_focus_on;
    bool can_focus_off;
    bool requesting_stop;
    bool requesting_abort;
    bool requesting_focus_off;
} panel_button_permissions_t;

typedef struct {
    system_state_t state;
    panel_fake_scene_t scene;
    panel_owner_t owner;
    panel_mode_t mode;
    int progress;
    uint32_t job_seconds;
    uint32_t seq;
    uint32_t event_id;
    uint32_t job_id;
    uint32_t received_size;
    uint32_t total_size;
    uint32_t executed_lines;
    uint32_t total_lines;
    uint32_t cache_free;
    bool rx_connected;
    bool tx_connected;
    bool host_connected;
    bool sle_connected;
    bool sd_mounted;
    bool focus_active;
    bool laser_output_active;
    char job_name[32];
    char last_error[32];
} panel_model_t;

extern panel_model_t g_model;

void panel_model_init(void);
void panel_model_set_state(system_state_t state);
void panel_model_set_scene(panel_fake_scene_t scene);
void panel_model_toggle_primary_mode(void);
void panel_model_next_scene(void);
void panel_model_set_progress(int pct);
void panel_model_tick(void);
void panel_model_request_stop(void);
void panel_model_request_abort(void);
void panel_model_request_focus_off(void);
void panel_model_get_button_permissions(panel_button_permissions_t *out);
const char *panel_model_state_text(system_state_t state);
const char *panel_model_owner_text(panel_owner_t owner);
const char *panel_model_mode_text(panel_mode_t mode);
const char *panel_model_scene_text(panel_fake_scene_t scene);

#endif
