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
    SYS_STATE_RECEIVING,
    SYS_STATE_READY,
    SYS_STATE_RUNNING,
    SYS_STATE_DONE,
    SYS_STATE_ERROR,
    SYS_STATE_LINK_LOST,
    SYS_STATE_COUNT
} system_state_t;

typedef struct {
    system_state_t state;
    int progress;
    uint32_t job_seconds;
    bool rx_connected;
    bool sle_connected;
    bool sd_mounted;
    char job_name[32];
} panel_model_t;

extern panel_model_t g_model;

void panel_model_init(void);
void panel_model_set_state(system_state_t state);
void panel_model_set_progress(int pct);
void panel_model_tick(void);

#endif
