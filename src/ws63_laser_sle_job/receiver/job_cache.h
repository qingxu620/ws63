/**
 * @file job_cache.h
 * @brief In-RAM G-code job cache for first-stage offline execution.
 */
#ifndef WS63_LASER_SLE_JOB_CACHE_H
#define WS63_LASER_SLE_JOB_CACHE_H

#include "protocol.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void job_cache_init(void);
void job_cache_clear(void);
sle_job_status_t job_cache_begin(uint32_t job_id, uint32_t total_size, uint16_t expected_crc);
sle_job_status_t job_cache_write(uint32_t job_id, uint32_t offset, const uint8_t *data, uint16_t len);
sle_job_status_t job_cache_finish(uint32_t job_id, uint32_t total_size, uint16_t expected_crc);
const uint8_t *job_cache_data(void);
uint32_t job_cache_size(void);
uint32_t job_cache_received(void);
uint32_t job_cache_total_size(void);
uint32_t job_cache_free(void);
uint32_t job_cache_job_id(void);
uint16_t job_cache_crc(void);
bool job_cache_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_SLE_JOB_CACHE_H */
