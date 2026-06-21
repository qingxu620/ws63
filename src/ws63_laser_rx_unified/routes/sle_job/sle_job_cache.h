/**
 * @file job_cache.h
 * @brief Circular-buffer G-code job cache for sliding-window execution.
 */
#ifndef WS63_LASER_RX_UNIFIED_SLE_JOB_CACHE_H
#define WS63_LASER_RX_UNIFIED_SLE_JOB_CACHE_H

#include "sle_job_protocol.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void sle_job_cache_init(void);
void sle_job_cache_clear(void);
sle_job_status_t sle_job_cache_begin(uint32_t job_id, uint32_t total_size, uint16_t expected_crc);
sle_job_status_t sle_job_cache_write(uint32_t job_id, uint32_t offset, const uint8_t *data, uint16_t len);
sle_job_status_t sle_job_cache_finish(uint32_t job_id, uint32_t total_size, uint16_t expected_crc);

uint32_t sle_job_cache_size(void);
uint32_t sle_job_cache_received(void);
uint32_t sle_job_cache_consumed(void);
uint32_t sle_job_cache_total_size(void);
uint32_t sle_job_cache_free(void);
uint32_t sle_job_cache_available(void);
uint32_t sle_job_cache_job_id(void);
uint16_t sle_job_cache_crc(void);
bool sle_job_cache_is_ready(void);

int sle_job_cache_read_byte(void);
void sle_job_cache_set_all_received(void);
bool sle_job_cache_is_all_received(void);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_UNIFIED_SLE_JOB_CACHE_H */
