/**
 * @file panel_model.c
 * @brief Local state model for RX-sourced panel status and offline control UI.
 */
#include "panel_model.h"
#include "soc_osal.h"
#include <stdio.h>
#include <string.h>

panel_model_t g_model;
static uint32_t g_live_job_start_tick_ms;
static bool g_live_job_timer_active;

#define PANEL_MODEL_TOTAL_SIZE_MAX UINT32_MAX

enum {
    PANEL_FAKE_TOTAL_SIZE = 131072U,
    PANEL_FAKE_TOTAL_LINES = 1000U,
    PANEL_FAKE_CACHE_SIZE = 102400U,
};

enum {
    PANEL_MODEL_STATUS_FLAG_FOCUS_ACTIVE = 0x01U,
    PANEL_MODEL_STATUS_FLAG_LASER_ACTIVE = 0x02U,
    PANEL_MODEL_STATUS_FLAG_ANY_LINK = 0x08U,
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

static void apply_status_flags(uint8_t flags)
{
    g_model.rx_connected = (flags & PANEL_MODEL_STATUS_FLAG_ANY_LINK) != 0U;
    g_model.sle_connected = g_model.tx_connected || g_model.rx_connected;
    g_model.focus_active = (flags & PANEL_MODEL_STATUS_FLAG_FOCUS_ACTIVE) != 0U;
    g_model.laser_output_active = (flags & PANEL_MODEL_STATUS_FLAG_LASER_ACTIVE) != 0U;
}

enum {
    RX_JOB_STATE_IDLE = 0U,
    RX_JOB_STATE_RECEIVING = 1U,
    RX_JOB_STATE_READY = 2U,
    RX_JOB_STATE_EXECUTING = 3U,
    RX_JOB_STATE_PAUSED = 4U,
    RX_JOB_STATE_ABORTED = 5U,
    RX_JOB_STATE_ERROR = 6U,
};

static bool state_is_offline_active(system_state_t state)
{
    return state == SYS_STATE_SENDING ||
           state == SYS_STATE_RUNNING ||
           state == SYS_STATE_PAUSED ||
           state == SYS_STATE_REQUESTING_STOP ||
           state == SYS_STATE_REQUESTING_ABORT ||
           state == SYS_STATE_REQUESTING_FOCUS_OFF;
}

static bool model_is_screen_offline_session(void)
{
    return g_model.view_mode == PANEL_VIEW_OFFLINE &&
           g_model.owner == PANEL_OWNER_SCREEN &&
           g_model.mode == PANEL_MODE_OFFLINE &&
           state_is_offline_active(g_model.state);
}

static bool job_id_matches_local(uint32_t job_id)
{
    return job_id == 0U || g_model.job_id == 0U || job_id == g_model.job_id;
}

static bool should_hold_local_request(system_state_t state, uint8_t job_state)
{
    if (state == SYS_STATE_REQUESTING_STOP) {
        return job_state != RX_JOB_STATE_IDLE &&
               job_state != RX_JOB_STATE_PAUSED &&
               job_state != RX_JOB_STATE_ABORTED &&
               job_state != RX_JOB_STATE_ERROR;
    }

    if (state == SYS_STATE_REQUESTING_ABORT) {
        return job_state != RX_JOB_STATE_IDLE &&
               job_state != RX_JOB_STATE_ABORTED &&
               job_state != RX_JOB_STATE_ERROR;
    }

    return false;
}

static bool state_uses_job_timer(system_state_t state)
{
    return state == SYS_STATE_RECEIVING ||
           state == SYS_STATE_SENDING ||
           state == SYS_STATE_RUNNING;
}

static void update_progress_fields(void)
{
    g_model.progress = clamp_pct(g_model.progress);
    g_model.total_size = clamp_u32(g_model.total_size, 0, PANEL_MODEL_TOTAL_SIZE_MAX);
    g_model.total_lines = clamp_u32(g_model.total_lines, 0, PANEL_FAKE_TOTAL_LINES);

    if (g_model.state == SYS_STATE_READY || g_model.state == SYS_STATE_RUNNING ||
        g_model.state == SYS_STATE_DONE || g_model.state == SYS_STATE_PAUSED ||
        g_model.state == SYS_STATE_TERMINATED ||
        g_model.state == SYS_STATE_REQUESTING_STOP ||
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
        g_model.state == SYS_STATE_DONE || g_model.state == SYS_STATE_PAUSED ||
        g_model.state == SYS_STATE_TERMINATED ||
        state_is_requesting(g_model.state)) {
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
    panel_view_mode_t view_mode = g_model.view_mode;
    g_model.job_seconds = 0;
    g_live_job_start_tick_ms = 0;
    g_live_job_timer_active = false;
    g_model.seq++;
    g_model.event_id++;
    g_model.owner = PANEL_OWNER_NONE;
    g_model.mode = PANEL_MODE_IDLE;
    g_model.view_mode = view_mode;
    g_model.rx_connected = false;
    g_model.tx_connected = false;
    g_model.host_connected = false;
    g_model.sle_connected = false;
    g_model.sd_mounted = false;
    g_model.focus_active = false;
    g_model.laser_output_active = false;
    g_model.live_status_active = false;
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
    g_model.view_mode = PANEL_VIEW_ONLINE;
    panel_model_set_scene(PANEL_SCENE_IDLE_NONE);
}

void panel_model_set_state(system_state_t state)
{
    if (state >= SYS_STATE_COUNT) return;
    g_model.state = state;
    if (state == SYS_STATE_NO_JOB) {
        g_model.scene = PANEL_SCENE_IDLE_NONE;
        g_model.focus_active = false;
        g_model.laser_output_active = false;
        g_model.progress = 0;
    } else if (state == SYS_STATE_ERROR) {
        g_model.mode = PANEL_MODE_ERROR;
        g_model.focus_active = false;
        g_model.laser_output_active = false;
    } else if (state == SYS_STATE_LINK_LOST) {
        g_model.mode = PANEL_MODE_LINK_LOST;
    }
    g_model.seq++;
    g_model.event_id++;
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
        g_model.view_mode = PANEL_VIEW_ONLINE;
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
        g_model.view_mode = PANEL_VIEW_ONLINE;
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
        g_model.view_mode = PANEL_VIEW_ONLINE;
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
        g_model.view_mode = PANEL_VIEW_ONLINE;
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
        g_model.view_mode = PANEL_VIEW_ONLINE;
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
        g_model.view_mode = PANEL_VIEW_ONLINE;
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
        g_model.view_mode = PANEL_VIEW_OFFLINE;
        g_model.state = SYS_STATE_BROWSING;
        g_model.owner = PANEL_OWNER_SCREEN;
        g_model.mode = PANEL_MODE_OFFLINE;
        g_model.sd_mounted = true;
        g_model.job_id = 2;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "R-C.nc");
        break;
    case PANEL_SCENE_SCREEN_SENDING:
        g_model.view_mode = PANEL_VIEW_OFFLINE;
        g_model.state = SYS_STATE_SENDING;
        g_model.owner = PANEL_OWNER_SCREEN;
        g_model.mode = PANEL_MODE_OFFLINE;
        g_model.sd_mounted = true;
        g_model.job_id = 2;
        g_model.progress = 28;
        g_model.total_size = PANEL_FAKE_TOTAL_SIZE;
        g_model.total_lines = PANEL_FAKE_TOTAL_LINES;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "R-C.nc");
        break;
    case PANEL_SCENE_SCREEN_RUNNING:
        g_model.view_mode = PANEL_VIEW_OFFLINE;
        g_model.state = SYS_STATE_RUNNING;
        g_model.owner = PANEL_OWNER_SCREEN;
        g_model.mode = PANEL_MODE_OFFLINE;
        g_model.sd_mounted = true;
        g_model.job_id = 2;
        g_model.progress = 62;
        g_model.total_size = PANEL_FAKE_TOTAL_SIZE;
        g_model.total_lines = PANEL_FAKE_TOTAL_LINES;
        g_model.laser_output_active = true;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "R-C.nc");
        break;
    case PANEL_SCENE_SCREEN_DONE:
        g_model.view_mode = PANEL_VIEW_OFFLINE;
        g_model.state = SYS_STATE_DONE;
        g_model.owner = PANEL_OWNER_SCREEN;
        g_model.mode = PANEL_MODE_OFFLINE;
        g_model.sd_mounted = true;
        g_model.job_id = 2;
        g_model.progress = 100;
        g_model.total_size = PANEL_FAKE_TOTAL_SIZE;
        g_model.total_lines = PANEL_FAKE_TOTAL_LINES;
        snprintf(g_model.job_name, sizeof(g_model.job_name), "R-C.nc");
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
}

void panel_model_toggle_primary_mode(void)
{
    if (g_model.view_mode == PANEL_VIEW_OFFLINE) {
        g_model.view_mode = PANEL_VIEW_ONLINE;
    } else {
        g_model.view_mode = PANEL_VIEW_OFFLINE;
        if (g_model.state == SYS_STATE_NO_JOB && !g_model.tx_connected) {
            g_model.state = SYS_STATE_BROWSING;
            g_model.owner = PANEL_OWNER_SCREEN;
            g_model.mode = PANEL_MODE_OFFLINE;
        }
    }

    g_model.event_id++;
    g_model.seq++;
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

}

void panel_model_request_stop(void)
{
    if (g_model.view_mode == PANEL_VIEW_ONLINE) {
        return;
    }
    if (g_model.view_mode == PANEL_VIEW_OFFLINE && !g_model.tx_connected) {
        g_model.owner = PANEL_OWNER_SCREEN;
        g_model.mode = PANEL_MODE_OFFLINE;
        g_model.host_connected = false;
        g_model.sd_mounted = true;
    }
    g_model.state = SYS_STATE_REQUESTING_STOP;
    g_model.laser_output_active = false;
    g_model.seq++;
    g_model.event_id++;
}

void panel_model_request_abort(void)
{
    if (g_model.view_mode == PANEL_VIEW_ONLINE) {
        return;
    }
    if (g_model.view_mode == PANEL_VIEW_OFFLINE && !g_model.tx_connected) {
        g_model.owner = PANEL_OWNER_SCREEN;
        g_model.mode = PANEL_MODE_OFFLINE;
        g_model.host_connected = false;
        g_model.sd_mounted = true;
    }
    g_model.state = SYS_STATE_REQUESTING_ABORT;
    g_model.focus_active = false;
    g_model.laser_output_active = false;
    g_model.seq++;
    g_model.event_id++;
}

void panel_model_request_focus_off(void)
{
    if (g_model.view_mode == PANEL_VIEW_ONLINE) {
        return;
    }
    g_model.state = SYS_STATE_REQUESTING_FOCUS_OFF;
    g_model.focus_active = false;
    g_model.laser_output_active = false;
    g_model.seq++;
    g_model.event_id++;
}

void panel_model_mark_focus_ack(bool active)
{
    g_model.focus_active = active;
    if (!active) {
        g_model.laser_output_active = false;
    } else {
        g_model.laser_output_active = true;
    }
    g_model.seq++;
    g_model.event_id++;
}

void panel_model_set_transport_links(bool tx_connected, bool rx_connected)
{
    bool changed = (g_model.tx_connected != tx_connected) ||
                   (g_model.rx_connected != rx_connected) ||
                   (g_model.sle_connected != (tx_connected || rx_connected));
    g_model.tx_connected = tx_connected;
    g_model.rx_connected = rx_connected;
    g_model.sle_connected = tx_connected || rx_connected;
    if (tx_connected) {
        g_model.host_connected = true;
    } else if (g_model.owner != PANEL_OWNER_HOST) {
        g_model.host_connected = false;
    }
    if (changed) {
        g_model.seq++;
        g_model.event_id++;
    }
}

void panel_model_select_offline_file(const char *name, uint32_t size_bytes, uint32_t line_count)
{
    panel_view_mode_t view_mode = g_model.view_mode;
    reset_common();
    g_model.view_mode = PANEL_VIEW_OFFLINE;
    if (view_mode == PANEL_VIEW_ONLINE) {
        g_model.view_mode = PANEL_VIEW_OFFLINE;
    }
    g_model.scene = PANEL_SCENE_SCREEN_BROWSING;
    g_model.state = SYS_STATE_READY;
    g_model.owner = PANEL_OWNER_SCREEN;
    g_model.mode = PANEL_MODE_OFFLINE;
    g_model.sd_mounted = true;
    g_model.job_id = 2;
    g_model.total_size = clamp_u32(size_bytes, 0, PANEL_MODEL_TOTAL_SIZE_MAX);
    g_model.total_lines = clamp_u32(line_count, 0, PANEL_FAKE_TOTAL_LINES);
    g_model.received_size = 0;
    g_model.executed_lines = 0;
    g_model.cache_free = PANEL_FAKE_CACHE_SIZE;
    g_model.progress = 0;
    g_model.live_status_active = false;
    snprintf(g_model.job_name, sizeof(g_model.job_name), "%s",
             (name != NULL && name[0] != '\0') ? name : "SD_JOB");
    snprintf(g_model.last_error, sizeof(g_model.last_error), "无");

}

void panel_model_start_offline_selected(void)
{
    if (g_model.owner != PANEL_OWNER_SCREEN ||
        g_model.mode != PANEL_MODE_OFFLINE ||
        g_model.state != SYS_STATE_READY) {
        osal_printk("[MODEL] offline start rejected owner=%d mode=%d state=%d\r\n",
                    g_model.owner, g_model.mode, g_model.state);
        return;
    }

    g_model.state = SYS_STATE_SENDING;
    g_model.progress = 0;
    g_model.received_size = 0;
    g_model.executed_lines = 0;
    g_model.job_seconds = 0;
    g_model.seq++;
    g_model.event_id++;
    update_progress_fields();
}

void panel_model_offline_upload_begin(const char *name, uint32_t size_bytes, uint32_t line_count)
{
    reset_common();
    g_model.view_mode = PANEL_VIEW_OFFLINE;
    g_model.scene = PANEL_SCENE_SCREEN_SENDING;
    g_model.state = SYS_STATE_SENDING;
    g_model.owner = PANEL_OWNER_SCREEN;
    g_model.mode = PANEL_MODE_OFFLINE;
    g_model.sd_mounted = true;
    g_model.sle_connected = true;
    g_model.rx_connected = true;
    g_model.job_id = 2;
    g_model.total_size = clamp_u32(size_bytes, 0, PANEL_MODEL_TOTAL_SIZE_MAX);
    g_model.total_lines = clamp_u32(line_count, 0, PANEL_FAKE_TOTAL_LINES);
    g_model.received_size = 0;
    g_model.executed_lines = 0;
    g_model.progress = 0;
    g_model.live_status_active = true;
    snprintf(g_model.job_name, sizeof(g_model.job_name), "%s",
             (name != NULL && name[0] != '\0') ? name : "SD_JOB");
    snprintf(g_model.last_error, sizeof(g_model.last_error), "无");
    update_progress_fields();
}

void panel_model_offline_upload_progress(uint32_t received_size)
{
    if (g_model.owner != PANEL_OWNER_SCREEN || g_model.mode != PANEL_MODE_OFFLINE) {
        return;
    }
    if (g_model.state != SYS_STATE_RUNNING &&
        g_model.state != SYS_STATE_PAUSED &&
        !state_is_requesting(g_model.state)) {
        g_model.state = SYS_STATE_SENDING;
    }
    g_model.received_size = clamp_u32(received_size, 0, g_model.total_size);
    g_model.progress = (g_model.total_size > 0U) ?
        (int)(((uint64_t)g_model.received_size * 100U) / g_model.total_size) : 0;
    g_model.progress = clamp_pct(g_model.progress);
    g_model.cache_free = (g_model.total_size > g_model.received_size) ?
        (g_model.total_size - g_model.received_size) : 0;
    g_model.seq++;
}

void panel_model_offline_execution_started(void)
{
    if (g_model.owner != PANEL_OWNER_SCREEN || g_model.mode != PANEL_MODE_OFFLINE) {
        return;
    }
    g_model.scene = PANEL_SCENE_SCREEN_RUNNING;
    g_model.state = SYS_STATE_RUNNING;
    g_model.progress = 0;
    g_model.executed_lines = 0;
    g_model.received_size = g_model.total_size;
    g_model.job_seconds = 0;
    g_model.seq++;
    g_model.event_id++;
}

void panel_model_offline_paused(void)
{
    if (g_model.owner != PANEL_OWNER_SCREEN || g_model.mode != PANEL_MODE_OFFLINE) {
        return;
    }
    g_model.state = SYS_STATE_PAUSED;
    g_model.laser_output_active = false;
    g_model.seq++;
    g_model.event_id++;
}

void panel_model_offline_resumed(void)
{
    if (g_model.owner != PANEL_OWNER_SCREEN || g_model.mode != PANEL_MODE_OFFLINE) {
        return;
    }
    g_model.state = SYS_STATE_RUNNING;
    g_model.seq++;
    g_model.event_id++;
}

void panel_model_offline_aborted(void)
{
    if (g_model.owner != PANEL_OWNER_SCREEN || g_model.mode != PANEL_MODE_OFFLINE) {
        return;
    }
    g_model.scene = PANEL_SCENE_IDLE_NONE;
    g_model.state = SYS_STATE_TERMINATED;
    g_model.focus_active = false;
    g_model.laser_output_active = false;
    g_model.seq++;
    g_model.event_id++;
}

void panel_model_offline_job_done(void)
{
    if (g_model.owner != PANEL_OWNER_SCREEN || g_model.mode != PANEL_MODE_OFFLINE) {
        return;
    }
    g_model.scene = PANEL_SCENE_SCREEN_DONE;
    g_model.state = SYS_STATE_DONE;
    g_model.progress = 100;
    g_model.received_size = g_model.total_size;
    g_model.executed_lines = g_model.total_lines;
    g_model.focus_active = false;
    g_model.laser_output_active = false;
    g_model.seq++;
    g_model.event_id++;
}

void panel_model_offline_error(const char *error)
{
    panel_view_mode_t view_mode = g_model.view_mode;
    reset_common();
    g_model.view_mode = (view_mode == PANEL_VIEW_ONLINE) ? PANEL_VIEW_OFFLINE : view_mode;
    g_model.scene = PANEL_SCENE_HOST_ERROR;
    g_model.state = SYS_STATE_ERROR;
    g_model.owner = PANEL_OWNER_SCREEN;
    g_model.mode = PANEL_MODE_ERROR;
    g_model.sd_mounted = true;
    g_model.focus_active = false;
    g_model.laser_output_active = false;
    g_model.live_status_active = true;
    snprintf(g_model.job_name, sizeof(g_model.job_name), "SD_JOB");
    snprintf(g_model.last_error, sizeof(g_model.last_error), "%s",
             (error != NULL && error[0] != '\0') ? error : "OFFLINE_ERR");
}

void panel_model_apply_rx_panel_status(uint8_t owner, uint8_t mode, uint8_t job_state,
                                       uint8_t flags, uint32_t seq, uint32_t job_id,
                                       uint32_t received_size, uint32_t total_size,
                                       uint32_t executed_lines, uint32_t cache_free,
                                       uint32_t last_error, uint32_t tick_ms)
{
    system_state_t prev_state = g_model.state;
    uint32_t prev_job_id = g_model.job_id;
    bool keep_local_offline_selection =
        (g_model.view_mode == PANEL_VIEW_OFFLINE &&
         g_model.owner == PANEL_OWNER_SCREEN &&
         g_model.mode == PANEL_MODE_OFFLINE &&
         g_model.state == SYS_STATE_READY &&
         owner == 0U && mode == 0U && job_state == 0U && job_id == 0U);
    bool remap_to_screen_offline =
        model_is_screen_offline_session() &&
        owner == PANEL_OWNER_HOST &&
        job_id_matches_local(job_id) &&
        (mode == PANEL_MODE_IDLE || mode == PANEL_MODE_ONLINE ||
         mode == PANEL_MODE_ERROR || mode == PANEL_MODE_LINK_LOST);
    bool hold_local_request = remap_to_screen_offline &&
        should_hold_local_request(prev_state, job_state);

    if (keep_local_offline_selection) {
        g_model.live_status_active = true;
        apply_status_flags(flags);
        g_model.cache_free = clamp_u32(cache_free, 0, PANEL_FAKE_CACHE_SIZE);
        g_model.seq++;
        g_model.event_id++;
        return;
    }

    if (remap_to_screen_offline) {
        owner = PANEL_OWNER_SCREEN;
        if (mode == PANEL_MODE_IDLE || mode == PANEL_MODE_ONLINE) {
            mode = PANEL_MODE_OFFLINE;
        }
    }

    g_model.scene = PANEL_SCENE_IDLE_NONE;
    g_model.live_status_active = true;
    g_model.seq = seq;
    g_model.event_id++;

    switch (owner) {
    case 1: g_model.owner = PANEL_OWNER_HOST; break;
    case 2: g_model.owner = PANEL_OWNER_SCREEN; break;
    default: g_model.owner = PANEL_OWNER_NONE; break;
    }

    switch (mode) {
    case 1: g_model.mode = PANEL_MODE_ONLINE; break;
    case 2: g_model.mode = PANEL_MODE_OFFLINE; break;
    case 3: g_model.mode = PANEL_MODE_ERROR; break;
    case 4: g_model.mode = PANEL_MODE_LINK_LOST; break;
    default: g_model.mode = PANEL_MODE_IDLE; break;
    }

    switch (job_state) {
    case 1:
        g_model.state = SYS_STATE_RECEIVING;
        break;
    case 2:
        g_model.state = SYS_STATE_READY;
        break;
    case 3:
        g_model.state = SYS_STATE_RUNNING;
        break;
    case 4:
        g_model.state = SYS_STATE_PAUSED;
        break;
    case 5:
        g_model.state = SYS_STATE_TERMINATED;
        break;
    case 6:
        g_model.state = SYS_STATE_ERROR;
        g_model.mode = PANEL_MODE_ERROR;
        break;
    default:
        g_model.state = SYS_STATE_NO_JOB;
        break;
    }
    if (hold_local_request) {
        g_model.state = prev_state;
    }

    g_model.job_id = job_id;
    g_model.received_size = received_size;
    g_model.total_size = total_size;
    g_model.executed_lines = executed_lines;
    g_model.cache_free = cache_free;
    g_model.total_lines = (executed_lines > 0U) ? executed_lines : PANEL_FAKE_TOTAL_LINES;
    g_model.host_connected = (g_model.owner == PANEL_OWNER_HOST);
    g_model.sd_mounted = (g_model.owner == PANEL_OWNER_SCREEN);
    apply_status_flags(flags);

    if (state_uses_job_timer(g_model.state)) {
        if (tick_ms != 0U) {
            if (!g_live_job_timer_active || !state_uses_job_timer(prev_state) ||
                prev_job_id != job_id) {
                g_live_job_start_tick_ms = tick_ms;
                g_live_job_timer_active = true;
                g_model.job_seconds = 0;
            } else if (tick_ms >= g_live_job_start_tick_ms) {
                g_model.job_seconds = (tick_ms - g_live_job_start_tick_ms) / 1000U;
            } else {
                g_live_job_start_tick_ms = tick_ms;
                g_model.job_seconds = 0;
            }
        } else if (!state_uses_job_timer(prev_state) || prev_job_id != job_id) {
            g_live_job_timer_active = false;
            g_model.job_seconds = 0;
        }
    } else {
        g_live_job_timer_active = false;
        g_live_job_start_tick_ms = 0;
        if (g_model.state == SYS_STATE_NO_JOB || g_model.state == SYS_STATE_ERROR ||
            g_model.state == SYS_STATE_LINK_LOST) {
            g_model.job_seconds = 0;
        }
    }

    if (total_size > 0U) {
        g_model.progress = (int)(((uint64_t)received_size * 100U) / total_size);
    } else {
        g_model.progress = 0;
    }
    g_model.progress = clamp_pct(g_model.progress);

    if (job_id != 0U) {
        const char *prefix = (g_model.owner == PANEL_OWNER_HOST &&
                              g_model.mode == PANEL_MODE_ONLINE) ? "HOST_JOB" : "RX_JOB";
        snprintf(g_model.job_name, sizeof(g_model.job_name), "%s_%03u",
                 prefix, (unsigned int)job_id);
    } else {
        snprintf(g_model.job_name, sizeof(g_model.job_name), "暂无任务");
    }
    if (last_error != 0U || g_model.state == SYS_STATE_ERROR) {
        snprintf(g_model.last_error, sizeof(g_model.last_error), "RX_ERR_%u", (unsigned int)last_error);
    } else {
        snprintf(g_model.last_error, sizeof(g_model.last_error), "无");
    }

    g_model.total_size = clamp_u32(g_model.total_size, 0, PANEL_MODEL_TOTAL_SIZE_MAX);
    g_model.received_size = clamp_u32(g_model.received_size, 0, g_model.total_size);
    g_model.cache_free = clamp_u32(g_model.cache_free, 0, PANEL_FAKE_CACHE_SIZE);
}

void panel_model_get_button_permissions(panel_button_permissions_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));

    out->requesting_stop = (g_model.state == SYS_STATE_REQUESTING_STOP);
    out->requesting_abort = (g_model.state == SYS_STATE_REQUESTING_ABORT);
    out->requesting_focus_off = (g_model.state == SYS_STATE_REQUESTING_FOCUS_OFF);
    if (g_model.view_mode == PANEL_VIEW_ONLINE) {
        return;
    }

    bool link_bad = (g_model.mode == PANEL_MODE_LINK_LOST || g_model.state == SYS_STATE_LINK_LOST);
    bool error = (g_model.mode == PANEL_MODE_ERROR || g_model.state == SYS_STATE_ERROR);
    bool requesting = state_is_requesting(g_model.state);
    bool active_job = (g_model.state == SYS_STATE_RECEIVING || g_model.state == SYS_STATE_SENDING ||
                       g_model.state == SYS_STATE_READY || g_model.state == SYS_STATE_RUNNING ||
                       g_model.state == SYS_STATE_PAUSED ||
                       g_model.state == SYS_STATE_TERMINATED ||
                       g_model.state == SYS_STATE_REQUESTING_STOP ||
                       g_model.state == SYS_STATE_REQUESTING_ABORT ||
                       g_model.state == SYS_STATE_REQUESTING_FOCUS_OFF);
    bool screen_owner = (g_model.owner == PANEL_OWNER_SCREEN && g_model.mode == PANEL_MODE_OFFLINE);
    bool screen_session_in_progress = screen_owner &&
        (g_model.state == SYS_STATE_RECEIVING || g_model.state == SYS_STATE_SENDING ||
         g_model.state == SYS_STATE_RUNNING || g_model.state == SYS_STATE_PAUSED ||
         g_model.state == SYS_STATE_REQUESTING_STOP ||
         g_model.state == SYS_STATE_REQUESTING_ABORT ||
         g_model.state == SYS_STATE_REQUESTING_FOCUS_OFF ||
         g_model.state == SYS_STATE_ERROR);
    bool screen_control_allowed = !g_model.tx_connected || screen_session_in_progress;

    out->can_start = screen_control_allowed && !requesting && !link_bad && !error &&
                     g_model.owner != PANEL_OWNER_HOST &&
                     (g_model.state == SYS_STATE_BROWSING || g_model.state == SYS_STATE_READY ||
                      g_model.state == SYS_STATE_DONE || g_model.state == SYS_STATE_PAUSED);
    out->can_stop = screen_control_allowed && !requesting && !link_bad &&
                    (g_model.state == SYS_STATE_RUNNING || g_model.state == SYS_STATE_RECEIVING ||
                     g_model.state == SYS_STATE_SENDING);
    out->can_abort = screen_control_allowed && !out->requesting_abort && !link_bad && (active_job || error);
    out->can_focus_on = screen_control_allowed && !requesting && !link_bad && !error &&
                        g_model.owner != PANEL_OWNER_HOST &&
                        (g_model.state == SYS_STATE_NO_JOB || g_model.state == SYS_STATE_BROWSING ||
                         g_model.state == SYS_STATE_DONE);
    out->can_focus_off = screen_control_allowed && !out->requesting_focus_off;
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
    case SYS_STATE_PAUSED: return "PAUSED";
    case SYS_STATE_REQUESTING_STOP: return "REQUESTING_STOP";
    case SYS_STATE_REQUESTING_ABORT: return "REQUESTING_ABORT";
    case SYS_STATE_REQUESTING_FOCUS_OFF: return "REQUESTING_FOCUS_OFF";
    case SYS_STATE_TERMINATED: return "TERMINATED";
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

const char *panel_model_view_mode_text(panel_view_mode_t view_mode)
{
    switch (view_mode) {
    case PANEL_VIEW_ONLINE: return "ONLINE_VIEW";
    case PANEL_VIEW_OFFLINE: return "OFFLINE_VIEW";
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

const char *panel_model_state_label(system_state_t state)
{
    switch (state) {
    case SYS_STATE_NO_JOB: return "空闲";
    case SYS_STATE_BROWSING: return "空闲";
    case SYS_STATE_RECEIVING: return "数据传输中";
    case SYS_STATE_SENDING: return "数据传输中";
    case SYS_STATE_READY: return "任务就绪";
    case SYS_STATE_RUNNING: return "正在执行";
    case SYS_STATE_DONE: return "执行完成";
    case SYS_STATE_PAUSED: return "已暂停";
    case SYS_STATE_REQUESTING_STOP: return "暂停中";
    case SYS_STATE_REQUESTING_ABORT: return "取消中";
    case SYS_STATE_REQUESTING_FOCUS_OFF: return "关光中";
    case SYS_STATE_TERMINATED: return "已终止";
    case SYS_STATE_ERROR: return "错误";
    case SYS_STATE_LINK_LOST: return "链路断开";
    default: return "未知";
    }
}

const char *panel_model_state_detail(system_state_t state)
{
    switch (state) {
    case SYS_STATE_NO_JOB: return "待机模式";
    case SYS_STATE_BROWSING: return "等待选择任务";
    case SYS_STATE_RECEIVING: return "正在下载任务数据";
    case SYS_STATE_SENDING: return "正在发送离线任务";
    case SYS_STATE_READY: return "代码已读取并校验";
    case SYS_STATE_RUNNING: return "等待 RX 完成";
    case SYS_STATE_DONE: return "任务完成，控制已释放";
    case SYS_STATE_PAUSED: return "执行已暂停";
    case SYS_STATE_REQUESTING_STOP: return "软件暂停执行中";
    case SYS_STATE_REQUESTING_ABORT: return "断光并取消任务中";
    case SYS_STATE_REQUESTING_FOCUS_OFF: return "调焦关光中";
    case SYS_STATE_TERMINATED: return "任务已终止";
    case SYS_STATE_ERROR: return "故障";
    case SYS_STATE_LINK_LOST: return "SLE/RX 链路断开";
    default: return "状态未知";
    }
}

const char *panel_model_owner_label(panel_owner_t owner)
{
    switch (owner) {
    case PANEL_OWNER_NONE: return "无";
    case PANEL_OWNER_HOST: return "HOST";
    case PANEL_OWNER_SCREEN: return "SCREEN";
    default: return "未知";
    }
}

const char *panel_model_mode_label(panel_mode_t mode)
{
    switch (mode) {
    case PANEL_MODE_IDLE: return "SLE";
    case PANEL_MODE_ONLINE: return "SLE";
    case PANEL_MODE_OFFLINE: return "离线";
    case PANEL_MODE_ERROR: return "故障";
    case PANEL_MODE_LINK_LOST: return "断链";
    default: return "未知";
    }
}

const char *panel_model_view_mode_label(panel_view_mode_t view_mode)
{
    switch (view_mode) {
    case PANEL_VIEW_ONLINE: return "在线镜像";
    case PANEL_VIEW_OFFLINE: return "离线任务";
    default: return "未知";
    }
}
