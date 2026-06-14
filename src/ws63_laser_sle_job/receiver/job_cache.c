/**
 * @file job_cache.c
 * @brief In-RAM G-code job cache.
 */
#include "job_cache.h"
#include "config.h"
#include "crc16.h"
#include "soc_osal.h"
#include <string.h>

static uint8_t g_job_buf[JOB_CACHE_SIZE];
static uint32_t g_job_id = 0;
static uint32_t g_total_size = 0;
static uint32_t g_received = 0;
static uint16_t g_expected_crc = 0;
static uint16_t g_running_crc = 0xFFFFU;
static bool g_receiving = false;
static bool g_ready = false;

void job_cache_init(void)
{
    job_cache_clear();
}

void job_cache_clear(void)
{
    g_job_id = 0;
    g_total_size = 0;
    g_received = 0;
    g_expected_crc = 0;
    g_running_crc = 0xFFFFU;
    g_receiving = false;
    g_ready = false;
}

sle_job_status_t job_cache_begin(uint32_t job_id, uint32_t total_size, uint16_t expected_crc)
{
    if (total_size == 0 || total_size > JOB_CACHE_SIZE) {
        return JOB_STATUS_NO_SPACE;
    }

    job_cache_clear();
    g_job_id = job_id;
    g_total_size = total_size;
    g_expected_crc = expected_crc;
    g_running_crc = 0xFFFFU;
    g_receiving = true;
    osal_printk("[JOB_CACHE] begin job=%u size=%u crc=0x%04x\r\n",
                (unsigned int)job_id, (unsigned int)total_size, expected_crc);
    return JOB_STATUS_OK;
}

sle_job_status_t job_cache_write(uint32_t job_id, uint32_t offset, const uint8_t *data, uint16_t len)
{
    if (!g_receiving || job_id != g_job_id) {
        return JOB_STATUS_BAD_JOB;
    }
    if (data == NULL || len == 0) {
        return JOB_STATUS_INTERNAL_ERROR;
    }
    if (offset != g_received) {
        osal_printk("[JOB_CACHE] bad offset job=%u got=%u expect=%u len=%u\r\n",
                    (unsigned int)job_id, (unsigned int)offset, (unsigned int)g_received, len);
        return JOB_STATUS_BAD_OFFSET;
    }
    if ((uint32_t)len > (g_total_size - g_received)) {
        return JOB_STATUS_NO_SPACE;
    }

    (void)memcpy(&g_job_buf[g_received], data, len);
    g_running_crc = job_crc16_ccitt_update(g_running_crc, data, len);
    g_received += len;
    return JOB_STATUS_OK;
}

sle_job_status_t job_cache_finish(uint32_t job_id, uint32_t total_size, uint16_t expected_crc)
{
    if (!g_receiving || job_id != g_job_id || total_size != g_total_size) {
        return JOB_STATUS_BAD_JOB;
    }
    if (g_received != g_total_size) {
        return JOB_STATUS_BAD_OFFSET;
    }
    if (expected_crc != 0 && expected_crc != g_expected_crc) {
        return JOB_STATUS_BAD_CRC;
    }
    if (g_expected_crc != 0 && g_running_crc != g_expected_crc) {
        osal_printk("[JOB_CACHE] crc fail job=%u calc=0x%04x expect=0x%04x\r\n",
                    (unsigned int)g_job_id, g_running_crc, g_expected_crc);
        return JOB_STATUS_BAD_CRC;
    }

    g_receiving = false;
    g_ready = true;
    osal_printk("[JOB_CACHE] ready job=%u size=%u crc=0x%04x\r\n",
                (unsigned int)g_job_id, (unsigned int)g_received, g_running_crc);
    return JOB_STATUS_OK;
}

const uint8_t *job_cache_data(void)
{
    return g_job_buf;
}

uint32_t job_cache_size(void)
{
    return JOB_CACHE_SIZE;
}

uint32_t job_cache_received(void)
{
    return g_received;
}

uint32_t job_cache_total_size(void)
{
    return g_total_size;
}

uint32_t job_cache_free(void)
{
    return (g_total_size > g_received) ? (g_total_size - g_received) : (JOB_CACHE_SIZE - g_received);
}

uint32_t job_cache_job_id(void)
{
    return g_job_id;
}

uint16_t job_cache_crc(void)
{
    return g_running_crc;
}

bool job_cache_is_ready(void)
{
    return g_ready;
}
