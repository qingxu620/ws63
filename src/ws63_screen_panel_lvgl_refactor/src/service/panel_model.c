/**
 * @file panel_model.c
 * @brief Local state model with demo state cycling.
 */
#include "panel_model.h"
#include "soc_osal.h"
#include <stdio.h>
#include <string.h>

panel_model_t g_model;

enum {
    PANEL_FAKE_TOTAL_SIZE = 131072U,
    PANEL_FAKE_TOTAL_LINES = 1000U,
    PANEL_FAKE_CACHE_SIZE = 131072U,
    PANEL_FAKE_REQUEST_DONE_SECONDS = 2U,
};

static uint32_t clamp_u32(uint32_t value, uint32_t min, uint32_t max)
{
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static int clamp_pct(int pct)
{
    if (pct < 0) return 0;
    if (pct > 100) return 100;
    return pct;
}

static bool state_is_requesting(system_state_t state)
{
    return state == SYS_STATE_REQUESTING_STOP ||
           state == SYS_STATE_REQUESTING_ABORT ||
           state == SYS_STATE_REQUESTING_FOCUS_OFF;
}

static void update_progress_fields(void)
{
    g_model.progress = clamp_pct(g_model.progress);
    g_model.total_size = clamp_u32(g_model.total_size, 0, PANEL_FAKE_TOTAL_SIZE);
    g_model.total_lines = clamp_u32(g_model.total_lines, 0, PANEL_FAKE_TOTAL_LINES);

    if (g_model.state == SYS_STATE_READY || g_model.state == SYS_STATE_RUNNING ||
        g_model.state == SYS_STATE_DONE || g_model.state == SYS_STATE_REQUESTING_STOP ||
        g_model.state == SYS_STATE_REQUESTING_ABORT || g_model.state == SYS_STATE_REQUESTING_FOCUS_OFF) {
        g_model.received_size = g_model.total_size;
    } else if ((g_model.state == SYS_STATE_RECEIVING || g_model.state == SYS_STATE_SENDING) &&
               g_model.total_size > 0) {
        g_model.received_size = (uint32_t)((uint64_t)g_model.total_size * (uint32_t)g_model.progress / 100U);
    } else {
        g_model.received_size = 0;
    }

    if (g_model.state == SYS_STATE_RECEIVING || g_model.state == SYS_STATE_SENDING ||
        g_model.state == SYS_STATE_READY || g_model.total_lines == 0) {
        g_model.executed_lines = 0;
    } else {
        g_model.executed_lines = (uint32_t)((uint64_t)g_model.total_lines * (uint32_t)g_model.progress / 100U);
    }

    if (g_model.state == SYS_STATE_RECEIVING || g_model.state == SYS_STATE_SENDING ||
        g_model.state == SYS_STATE_READY || g_model.state == SYS_STATE_RUNNING ||
        g_model.state == SYS_STATE_DONE || state_is_requesting(g_model.state)) {
        uint32_t used = (uint32_t)((uint64_t)PANEL_FAKE_CACHE_SIZE * (uint32_t)g_model.progress / 100U);
        g_model.cache_free = PANEL_FAKE_CACHE_SIZE - clamp_u32(used, 0, PANEL_FAKE_CACHE_SIZE);
    } else {
        g_model.cache_free = PANEL_FAKE_CACHE_SIZE;
    }

    g_model.received_size = clamp_u32(g_model.received_size, 0, g_model.total_size);
    g_model.executed_lines = clamp_u32(g_model.executed_lines, 0, g_model.total_lines);
    g_model.cache_free = clamp_u32(g_model.cache_free, 0, PANEL_FAKE_CACHE_SIZE);
}

static void reset_common(void)
{
    g_model.job_seconds = 0;
    g_model.seq++;
    g_model.event_id++;
    g_model.owner = PANEL_OWNER_NONE;
    g_model.mode = PANEL_MODE_IDLE;
    g_model.rx_connected = true;
    g_model.tx_connected = true;
    g_model.host_connected = false;
    g_model.sle_connected = true;
    g_model.sd_mounted = false;
    g_model.focus_active = false;
    g_model.laser_output_active = false;
    g_model.job_id = 0;
    g_model.progress = 0;
    g_model.total_size = 0;
    g_model.total_lines = 0;
    g_model.received_size = 0;
    g_model.executed_lines = 0;
    g_model.cache_free = PANEL_FAKE_CACHE_SIZE;
    snprintf(g_model.job_name, sizeof(g_model.job_name), "暂无任务");
    snprintf(g_model.last_error, sizeof(g_model.last_error), "无");
}

void panel_model_init(void)
{
    memset(&g_model, 0, sizeof(g_model));
    panel_model_set_scene(PANEL_SCENE_IDLE_NONE);
    osal_printk("[MODEL] init scene=IDLE_NONE\r\n");
}

void panel_model_set_state(system_state_t state)
{
    if (state >= SYS_STATE_COUNT) return;

    switch (state) {
    case SYS_STATE_NO_JOB: panel_model_set_scene(PANEL_SCENE_IDLE_NONE); return;
    case SYS_STATE_BROWSING: panel_model_set_scene(PANEL_SCENE_SCREEN_BROWSING); return;
    case SYS_STATE_RECEIVING: panel_model_set_scene(PANEL_SCENE_HOST_RECEIVING); return;
    case SYS_STATE_SENDING: panel_model_set_scene(PANEL_SCENE_SCREEN_SENDING); return;
    case SYS_STATE_READY: panel_model_set_scene(PANEL_SCENE_HOST_READY); return;
    case SYS_STATE_RUNNING: panel_model_set_scene(PANEL_SCENE_HOST_RUNNING); return;
    case SYS_STATE_DONE: panel_model_set_scene(PANEL_SCENE_HOST_DONE); return;
    case SYS_STATE_REQUESTING_STOP: panel_model_set_scene(PANEL_SCENE_REQUESTING_STOP); return;
    case SYS_STATE_REQUESTING_ABORT: panel_model_set_scene(PANEL_SCENE_REQUESTING_ABORT); return;
    case SYS_STATE_REQUESTING_FOCUS_OFF: panel_model_set_scene(PANEL_SCENE_REQUESTING_FOCUS_OFF); return;
    case SYS_STATE_ERROR: panel_model_set_scene(PANEL_SCENE_HOST_ERROR); return;
    case SYS_STATE_LINK_LOST: panel_model_set_scene(PANEL_SCENE_HOST_LINK_LOST); return;
    default: return;
    }
}

void panel_model_set_scene(panel_fake_scene_t scene)
{
    if (scene >= PANEL_SCENE_COUNT) return;

    reset_common();
    g_model.scene = scene;

    switch (scene) {
    case PANEL_SCENE_IDLE_NONE:
        g_model.state = SYS_STATE_NO_JOB;
        break;
    case PANEL_SCENE_HOST_RECEIVING:
        g_model.state = SYS_STATE_RECEIVING;
        g_model.owner = PANEL_OWNER_HOST;
        g_model.mode = PANEL_MODE_ONLINE;
        g_model.host_connected = true;
        g_model.job_id = 1;
        g_model.progress = 35;
        g_model.total_size = PANEL_FAKE_TOTAL_SIZE;
        g_model.total_lines = PANEL_FAKE_TOTAL_LINES;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "HOST_JOB_001");
        break;
    case PANEL_SCENE_HOST_READY:
        g_model.state = SYS_STATE_READY;
        g_model.owner = PANEL_OWNER_HOST;
        g_model.mode = PANEL_MODE_ONLINE;
        g_model.host_connected = true;
        g_model.job_id = 1;
        g_model.total_size = PANEL_FAKE_TOTAL_SIZE;
        g_model.total_lines = PANEL_FAKE_TOTAL_LINES;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "HOST_JOB_001");
        break;
    case PANEL_SCENE_HOST_RUNNING:
        g_model.state = SYS_STATE_RUNNING;
        g_model.owner = PANEL_OWNER_HOST;
        g_model.mode = PANEL_MODE_ONLINE;
        g_model.host_connected = true;
        g_model.job_id = 1;
        g_model.progress = 48;
        g_model.total_size = PANEL_FAKE_TOTAL_SIZE;
        g_model.total_lines = PANEL_FAKE_TOTAL_LINES;
        g_model.laser_output_active = true;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "HOST_JOB_001");
        break;
    case PANEL_SCENE_HOST_DONE:
        g_model.state = SYS_STATE_DONE;
        g_model.owner = PANEL_OWNER_HOST;
        g_model.mode = PANEL_MODE_ONLINE;
        g_model.host_connected = true;
        g_model.job_id = 1;
        g_model.progress = 100;
        g_model.total_size = PANEL_FAKE_TOTAL_SIZE;
        g_model.total_lines = PANEL_FAKE_TOTAL_LINES;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "HOST_JOB_001");
        break;
    case PANEL_SCENE_HOST_ERROR:
        g_model.state = SYS_STATE_ERROR;
        g_model.owner = PANEL_OWNER_HOST;
        g_model.mode = PANEL_MODE_ERROR;
        g_model.host_connected = true;
        g_model.job_id = 1;
        g_model.total_size = PANEL_FAKE_TOTAL_SIZE;
        g_model.total_lines = PANEL_FAKE_TOTAL_LINES;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "HOST_JOB_001");
        snprintf(g_model.last_error, sizeof(g_model.last_error), "RX_ERR_TIMEOUT");
        break;
    case PANEL_SCENE_HOST_LINK_LOST:
        g_model.state = SYS_STATE_LINK_LOST;
        g_model.owner = PANEL_OWNER_HOST;
        g_model.mode = PANEL_MODE_LINK_LOST;
        g_model.host_connected = true;
        g_model.rx_connected = false;
        g_model.sle_connected = false;
        g_model.job_id = 1;
        g_model.total_size = PANEL_FAKE_TOTAL_SIZE;
        g_model.total_lines = PANEL_FAKE_TOTAL_LINES;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "HOST_JOB_001");
        snprintf(g_model.last_error, sizeof(g_model.last_error), "LINK_LOST");
        break;
    case PANEL_SCENE_SCREEN_BROWSING:
        g_model.state = SYS_STATE_BROWSING;
        g_model.owner = PANEL_OWNER_SCREEN;
        g_model.mode = PANEL_MODE_OFFLINE;
        g_model.sd_mounted = true;
        g_model.job_id = 2;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "SD_DEMO_001");
        break;
    case PANEL_SCENE_SCREEN_SENDING:
        g_model.state = SYS_STATE_SENDING;
        g_model.owner = PANEL_OWNER_SCREEN;
        g_model.mode = PANEL_MODE_OFFLINE;
        g_model.sd_mounted = true;
        g_model.job_id = 2;
        g_model.progress = 28;
        g_model.total_size = PANEL_FAKE_TOTAL_SIZE;
        g_model.total_lines = PANEL_FAKE_TOTAL_LINES;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "SD_DEMO_001");
        break;
    case PANEL_SCENE_SCREEN_RUNNING:
        g_model.state = SYS_STATE_RUNNING;
        g_model.owner = PANEL_OWNER_SCREEN;
        g_model.mode = PANEL_MODE_OFFLINE;
        g_model.sd_mounted = true;
        g_model.job_id = 2;
        g_model.progress = 62;
        g_model.total_size = PANEL_FAKE_TOTAL_SIZE;
        g_model.total_lines = PANEL_FAKE_TOTAL_LINES;
        g_model.laser_output_active = true;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "SD_DEMO_001");
        break;
    case PANEL_SCENE_SCREEN_DONE:
        g_model.state = SYS_STATE_DONE;
        g_model.owner = PANEL_OWNER_SCREEN;
        g_model.mode = PANEL_MODE_OFFLINE;
        g_model.sd_mounted = true;
        g_model.job_id = 2;
        g_model.progress = 100;
        g_model.total_size = PANEL_FAKE_TOTAL_SIZE;
        g_model.total_lines = PANEL_FAKE_TOTAL_LINES;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "SD_DEMO_001");
        break;
    case PANEL_SCENE_REQUESTING_STOP:
        g_model.state = SYS_STATE_REQUESTING_STOP;
        g_model.owner = PANEL_OWNER_HOST;
        g_model.mode = PANEL_MODE_ONLINE;
        g_model.host_connected = true;
        g_model.job_id = 1;
        g_model.progress = 58;
        g_model.total_size = PANEL_FAKE_TOTAL_SIZE;
        g_model.total_lines = PANEL_FAKE_TOTAL_LINES;
        g_model.laser_output_active = true;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "HOST_JOB_001");
        break;
    case PANEL_SCENE_REQUESTING_ABORT:
        g_model.state = SYS_STATE_REQUESTING_ABORT;
        g_model.owner = PANEL_OWNER_HOST;
        g_model.mode = PANEL_MODE_ONLINE;
        g_model.host_connected = true;
        g_model.job_id = 1;
        g_model.progress = 58;
        g_model.total_size = PANEL_FAKE_TOTAL_SIZE;
        g_model.total_lines = PANEL_FAKE_TOTAL_LINES;
        g_model.laser_output_active = true;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "HOST_JOB_001");
        break;
    case PANEL_SCENE_REQUESTING_FOCUS_OFF:
        g_model.state = SYS_STATE_REQUESTING_FOCUS_OFF;
        g_model.owner = PANEL_OWNER_HOST;
        g_model.mode = PANEL_MODE_ONLINE;
        g_model.host_connected = true;
        g_model.job_id = 1;
        g_model.progress = 58;
        g_model.total_size = PANEL_FAKE_TOTAL_SIZE;
        g_model.total_lines = PANEL_FAKE_TOTAL_LINES;
        g_model.focus_active = true;
        g_model.laser_output_active = true;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "HOST_JOB_001");
        break;
    default:
        break;
    }

    update_progress_fields();
    osal_printk("[MODEL] scene=%d state=%d owner=%d mode=%d progress=%d\r\n",
                g_model.scene, g_model.state, g_model.owner, g_model.mode, g_model.progress);
}

void panel_model_next_scene(void)
{
    panel_fake_scene_t next = (panel_fake_scene_t)(g_model.scene + 1);
    if (next >= PANEL_SCENE_COUNT) {
        next = PANEL_SCENE_IDLE_NONE;
    }
    panel_model_set_scene(next);
}

void panel_model_toggle_primary_mode(void)
{
    if (g_model.owner == PANEL_OWNER_SCREEN || g_model.mode == PANEL_MODE_OFFLINE) {
        panel_model_set_scene(PANEL_SCENE_HOST_READY);
    } else {
        panel_model_set_scene(PANEL_SCENE_SCREEN_BROWSING);
    }
}

void panel_model_set_progress(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_model.progress = pct;
    g_model.seq++;
    update_progress_fields();
}

void panel_model_tick(void)
{
    if (g_model.state == SYS_STATE_RUNNING || g_model.state == SYS_STATE_SENDING ||
        g_model.state == SYS_STATE_RECEIVING || state_is_requesting(g_model.state)) {
        g_model.job_seconds++;
        g_model.seq++;
    }

    if (state_is_requesting(g_model.state) &&
        g_model.job_seconds >= PANEL_FAKE_REQUEST_DONE_SECONDS) {
        osal_printk("[MODEL] fake request acknowledged state=%d -> IDLE_NONE\r\n",
                    g_model.state);
        panel_model_set_scene(PANEL_SCENE_IDLE_NONE);
    }
}

void panel_model_request_stop(void)
{
    panel_model_set_scene(PANEL_SCENE_REQUESTING_STOP);
}

void panel_model_request_abort(void)
{
    panel_model_set_scene(PANEL_SCENE_REQUESTING_ABORT);
}

void panel_model_request_focus_off(void)
{
    panel_model_set_scene(PANEL_SCENE_REQUESTING_FOCUS_OFF);
}

void panel_model_get_button_permissions(panel_button_permissions_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));

    out->requesting_stop = (g_model.state == SYS_STATE_REQUESTING_STOP);
    out->requesting_abort = (g_model.state == SYS_STATE_REQUESTING_ABORT);
    out->requesting_focus_off = (g_model.state == SYS_STATE_REQUESTING_FOCUS_OFF);

    bool link_bad = (g_model.mode == PANEL_MODE_LINK_LOST || g_model.state == SYS_STATE_LINK_LOST);
    bool error = (g_model.mode == PANEL_MODE_ERROR || g_model.state == SYS_STATE_ERROR);
    bool requesting = state_is_requesting(g_model.state);
    bool active_job = (g_model.state == SYS_STATE_RECEIVING || g_model.state == SYS_STATE_SENDING ||
                       g_model.state == SYS_STATE_READY || g_model.state == SYS_STATE_RUNNING ||
                       g_model.state == SYS_STATE_REQUESTING_STOP ||
                       g_model.state == SYS_STATE_REQUESTING_ABORT ||
                       g_model.state == SYS_STATE_REQUESTING_FOCUS_OFF);

    out->can_start = !requesting && !link_bad && !error &&
                     g_model.owner != PANEL_OWNER_HOST &&
                     (g_model.state == SYS_STATE_BROWSING || g_model.state == SYS_STATE_READY ||
                      g_model.state == SYS_STATE_DONE);
    out->can_stop = !requesting && !link_bad &&
                    (g_model.state == SYS_STATE_RUNNING || g_model.state == SYS_STATE_RECEIVING ||
                     g_model.state == SYS_STATE_SENDING);
    out->can_abort = !out->requesting_abort && !link_bad && (active_job || error);
    out->can_focus_on = !requesting && !link_bad && !error &&
                        g_model.owner != PANEL_OWNER_HOST &&
                        (g_model.state == SYS_STATE_NO_JOB || g_model.state == SYS_STATE_BROWSING ||
                         g_model.state == SYS_STATE_DONE);
    out->can_focus_off = !out->requesting_focus_off;
}

const char *panel_model_state_text(system_state_t state)
{
    switch (state) {
    case SYS_STATE_NO_JOB: return "IDLE";
    case SYS_STATE_BROWSING: return "BROWSING";
    case SYS_STATE_RECEIVING: return "RECEIVING";
    case SYS_STATE_SENDING: return "SENDING";
    case SYS_STATE_READY: return "JOB_READY";
    case SYS_STATE_RUNNING: return "EXECUTING";
    case SYS_STATE_DONE: return "DONE";
    case SYS_STATE_REQUESTING_STOP: return "REQUESTING_STOP";
    case SYS_STATE_REQUESTING_ABORT: return "REQUESTING_ABORT";
    case SYS_STATE_REQUESTING_FOCUS_OFF: return "REQUESTING_FOCUS_OFF";
    case SYS_STATE_ERROR: return "ERROR";
    case SYS_STATE_LINK_LOST: return "LINK_LOST";
    default: return "UNKNOWN";
    }
}

const char *panel_model_owner_text(panel_owner_t owner)
{
    switch (owner) {
    case PANEL_OWNER_NONE: return "NONE";
    case PANEL_OWNER_HOST: return "HOST";
    case PANEL_OWNER_SCREEN: return "SCREEN";
    default: return "UNKNOWN";
    }
}

const char *panel_model_mode_text(panel_mode_t mode)
{
    switch (mode) {
    case PANEL_MODE_IDLE: return "IDLE";
    case PANEL_MODE_ONLINE: return "ONLINE";
    case PANEL_MODE_OFFLINE: return "OFFLINE";
    case PANEL_MODE_ERROR: return "ERROR";
    case PANEL_MODE_LINK_LOST: return "LINK_LOST";
    default: return "UNKNOWN";
    }
}

const char *panel_model_scene_text(panel_fake_scene_t scene)
{
    switch (scene) {
    case PANEL_SCENE_IDLE_NONE: return "IDLE_NONE";
    case PANEL_SCENE_HOST_RECEIVING: return "HOST_RECEIVING";
    case PANEL_SCENE_HOST_READY: return "HOST_READY";
    case PANEL_SCENE_HOST_RUNNING: return "HOST_RUNNING";
    case PANEL_SCENE_HOST_DONE: return "HOST_DONE";
    case PANEL_SCENE_HOST_ERROR: return "HOST_ERROR";
    case PANEL_SCENE_HOST_LINK_LOST: return "HOST_LINK_LOST";
    case PANEL_SCENE_SCREEN_BROWSING: return "SCREEN_BROWSING";
    case PANEL_SCENE_SCREEN_SENDING: return "SCREEN_SENDING";
    case PANEL_SCENE_SCREEN_RUNNING: return "SCREEN_RUNNING";
    case PANEL_SCENE_SCREEN_DONE: return "SCREEN_DONE";
    case PANEL_SCENE_REQUESTING_STOP: return "REQUESTING_STOP";
    case PANEL_SCENE_REQUESTING_ABORT: return "REQUESTING_ABORT";
    case PANEL_SCENE_REQUESTING_FOCUS_OFF: return "REQUESTING_FOCUS_OFF";
    default: return "UNKNOWN";
    }
}
