/**
 * @file panel_offline_job.h
 * @brief Offline Screen-as-TX job sender.
 */
#ifndef PANEL_OFFLINE_JOB_H
#define PANEL_OFFLINE_JOB_H

#include "errcode.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

errcode_t panel_offline_job_init(void);
errcode_t panel_offline_job_start_selected(void);
bool panel_offline_job_is_busy(void);

#ifdef __cplusplus
}
#endif

#endif
