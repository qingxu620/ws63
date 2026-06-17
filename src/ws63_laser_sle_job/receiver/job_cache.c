/**
 * @file job_cache.c
 * @brief Circular-buffer G-code job cache for sliding-window execution.
 */
#include "job_cache.h"
#include "config.h"
#include "crc16.h"
#include "soc_osal.h"
#include <string.h>

static uint8_t g_cache[JOB_CACHE_SIZE];
static volatile uint32_t g_write_pos = 0;
static volatile uint32_t g_read_pos = 0;
static volatile uint32_t g_data_len = 0;

static uint32_t g_job_id = 0;
static uint32_t g_total_size = 0;
static uint32_t g_received = 0;
static uint32_t g_consumed = 0;
static uint16_t g_expected_crc = 0;
static uint16_t g_running_crc = 0xFFFFU;
static volatile bool g_receiving = false;
static volatile bool g_ready = false;
static volatile bool g_all_received = false;

static osal_mutex g_cache_mutex;
static bool g_mutex_ready = false;

void job_cache_init(void)
{
    if (osal_mutex_init(&g_cache_mutex) == OSAL_SUCCESS) {
        g_mutex_ready = true;
    }
    job_cache_clear();
}

void job_cache_clear(void)
{
    if (g_mutex_ready) {
        osal_mutex_lock(&g_cache_mutex);
    }
    g_write_pos = 0;
    g_read_pos = 0;
    g_data_len = 0;
    g_job_id = 0;
    g_total_size = 0;
    g_received = 0;
    g_consumed = 0;
    g_expected_crc = 0;
    g_running_crc = 0xFFFFU;
    g_receiving = false;
    g_ready = false;
    g_all_received = false;
    if (g_mutex_ready) {
        osal_mutex_unlock(&g_cache_mutex);
    }
}

sle_job_status_t job_cache_begin(uint32_t job_id, uint32_t total_size, uint16_t expected_crc)
{
    if (total_size == 0 || total_size > JOB_CACHE_SIZE) {
        return JOB_STATUS_NO_SPACE;
    }

    job_cache_clear();
    if (g_mutex_ready) {
        osal_mutex_lock(&g_cache_mutex);
    }
    g_job_id = job_id;
    g_total_size = total_size;
    g_expected_crc = expected_crc;
    g_running_crc = 0xFFFFU;
    g_receiving = true;
    if (g_mutex_ready) {
        osal_mutex_unlock(&g_cache_mutex);
    }
    osal_printk("[JOB_CACHE] begin job=%u size=%u crc=0x%04x buf=%u\r\n",
                (unsigned int)job_id, (unsigned int)total_size, expected_crc,
                (unsigned int)JOB_CACHE_SIZE);
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
    if ((uint32_t)len > job_cache_free()) {
        osal_printk("[JOB_CACHE] no space job=%u len=%u free=%u\r\n",
                    (unsigned int)job_id, (unsigned int)len, (unsigned int)job_cache_free());
        return JOB_STATUS_NO_SPACE;
    }

    if (g_mutex_ready) {
        osal_mutex_lock(&g_cache_mutex);
    }

    for (uint16_t i = 0; i < len; i++) {
        g_cache[g_write_pos] = data[i];
        g_write_pos = (g_write_pos + 1U) % JOB_CACHE_SIZE;
    }
    g_data_len += len;
    g_running_crc = job_crc16_ccitt_update(g_running_crc, data, len);
    g_received += len;

    if (g_mutex_ready) {
        osal_mutex_unlock(&g_cache_mutex);
    }
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

int job_cache_read_byte(void)
{
    if (g_data_len == 0) {
        return -1;
    }

    if (g_mutex_ready) {
        osal_mutex_lock(&g_cache_mutex);
    }

    uint8_t ch = g_cache[g_read_pos];
    g_read_pos = (g_read_pos + 1U) % JOB_CACHE_SIZE;
    g_data_len--;
    g_consumed++;

    if (g_mutex_ready) {
        osal_mutex_unlock(&g_cache_mutex);
    }
    return (int)ch;
}

uint32_t job_cache_size(void)
{
    return JOB_CACHE_SIZE;
}

uint32_t job_cache_received(void)
{
    return g_received;
}

uint32_t job_cache_consumed(void)
{
    return g_consumed;
}

uint32_t job_cache_total_size(void)
{
    return g_total_size;
}

uint32_t job_cache_free(void)
{
    return JOB_CACHE_SIZE - g_data_len;
}

uint32_t job_cache_available(void)
{
    return g_data_len;
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

void job_cache_set_all_received(void)
{
    g_all_received = true;
}

bool job_cache_is_all_received(void)
{
    return g_all_received;
}
