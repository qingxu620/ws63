/**
 * @file job_cache.c
 * @brief Circular-buffer G-code job cache for sliding-window execution.
 */
#include "sle_job_cache.h"
#include "sle_job_config.h"
#include "sle_job_crc16.h"
#include "soc_osal.h"
#include "systick.h"
#include <string.h>

static uint8_t g_cache[SLE_JOB_CACHE_SIZE];
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

void sle_job_cache_init(void)
{
    if (osal_mutex_init(&g_cache_mutex) == OSAL_SUCCESS) {
        g_mutex_ready = true;
    }
    sle_job_cache_clear();
}

void sle_job_cache_clear(void)
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

sle_job_status_t sle_job_cache_begin(uint32_t job_id, uint32_t total_size, uint16_t expected_crc)
{
    if (total_size == 0) {
        return SLE_JOB_STATUS_NO_SPACE;
    }

    sle_job_cache_clear();
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
    return SLE_JOB_STATUS_OK;
}

sle_job_status_t sle_job_cache_write(uint32_t job_id, uint32_t offset, const uint8_t *data, uint16_t len)
{
    uint32_t t_total = (uint32_t)uapi_systick_get_ms();
    if (!g_receiving || job_id != g_job_id) {
        osal_printk("[JOB_CACHE_WRITE_REJECT] reason=not_receiving receiving=%d job=%u expected=%u\r\n",
                    (int)g_receiving, (unsigned int)job_id, (unsigned int)g_job_id);
        return SLE_JOB_STATUS_BAD_JOB;
    }
    if (data == NULL || len == 0) {
        osal_printk("[JOB_CACHE_WRITE_REJECT] reason=null_or_zero data=%p len=%u\r\n",
                    (const void *)data, (unsigned int)len);
        return SLE_JOB_STATUS_INTERNAL_ERROR;
    }
    if (offset != g_received) {
        osal_printk("[JOB_CACHE_WRITE_REJECT] reason=bad_offset got=%u expect=%u len=%u\r\n",
                    (unsigned int)offset, (unsigned int)g_received, (unsigned int)len);
        return SLE_JOB_STATUS_BAD_OFFSET;
    }
    if (offset > g_total_size || (uint32_t)len > (g_total_size - offset)) {
        osal_printk("[JOB_CACHE_WRITE_REJECT] reason=past_total off=%u len=%u total=%u\r\n",
                    (unsigned int)offset, (unsigned int)len, (unsigned int)g_total_size);
        return SLE_JOB_STATUS_BAD_OFFSET;
    }
    if ((uint32_t)len > sle_job_cache_free()) {
        osal_printk("[JOB_CACHE_WRITE_REJECT] reason=no_space len=%u free=%u\r\n",
                    (unsigned int)len, (unsigned int)sle_job_cache_free());
        return SLE_JOB_STATUS_NO_SPACE;
    }

    uint32_t t_lock = (uint32_t)uapi_systick_get_ms();
    if (g_mutex_ready) {
        osal_mutex_lock(&g_cache_mutex);
    }
    uint32_t lock_ms = (uint32_t)uapi_systick_get_ms() - t_lock;
    uint32_t avail_before = g_data_len;
    uint32_t free_before = SLE_JOB_CACHE_SIZE - g_data_len;
    uint32_t rx_before = g_received;
    uint32_t consumed_before = g_consumed;

    uint32_t t_copy = (uint32_t)uapi_systick_get_ms();
    for (uint16_t i = 0; i < len; i++) {
        g_cache[g_write_pos] = data[i];
        g_write_pos = (g_write_pos + 1U) % SLE_JOB_CACHE_SIZE;
    }
    uint32_t copy_ms = (uint32_t)uapi_systick_get_ms() - t_copy;
    g_data_len += len;

    uint32_t t_crc = (uint32_t)uapi_systick_get_ms();
    g_running_crc = sle_job_crc16_ccitt_update(g_running_crc, data, len);
    uint32_t crc_ms = (uint32_t)uapi_systick_get_ms() - t_crc;
    g_received += len;

    uint32_t avail_after = g_data_len;
    uint32_t free_after = SLE_JOB_CACHE_SIZE - g_data_len;
    uint32_t rx_after = g_received;
    uint32_t consumed_after = g_consumed;

    uint32_t t_unlock = (uint32_t)uapi_systick_get_ms();
    if (g_mutex_ready) {
        osal_mutex_unlock(&g_cache_mutex);
    }
    uint32_t unlock_ms = (uint32_t)uapi_systick_get_ms() - t_unlock;
    uint32_t total_ms = (uint32_t)uapi_systick_get_ms() - t_total;
    if (total_ms >= SLE_JOB_TIMING_SLOW_MS || lock_ms >= SLE_JOB_TIMING_SLOW_MS ||
        copy_ms >= SLE_JOB_TIMING_SLOW_MS || crc_ms >= SLE_JOB_TIMING_SLOW_MS) {
        osal_printk("[JOB_CACHE_WRITE_TIMING] job=%u off=%u len=%u total_ms=%u lock_ms=%u "
                    "copy_ms=%u crc_ms=%u unlock_ms=%u rx=%u->%u consumed=%u->%u "
                    "avail=%u->%u free=%u->%u\r\n",
                    (unsigned int)job_id, (unsigned int)offset, (unsigned int)len,
                    (unsigned int)total_ms, (unsigned int)lock_ms,
                    (unsigned int)copy_ms, (unsigned int)crc_ms,
                    (unsigned int)unlock_ms, (unsigned int)rx_before,
                    (unsigned int)rx_after, (unsigned int)consumed_before,
                    (unsigned int)consumed_after, (unsigned int)avail_before,
                    (unsigned int)avail_after, (unsigned int)free_before,
                    (unsigned int)free_after);
    }
    return SLE_JOB_STATUS_OK;
}

bool sle_job_cache_is_duplicate_data(uint32_t job_id, uint32_t offset, uint16_t len)
{
    if (job_id != g_job_id || len == 0U || offset > g_received) {
        return false;
    }

    uint32_t end = offset + (uint32_t)len;
    if (end < offset) {
        return false;
    }

    return end <= g_received;
}

sle_job_status_t sle_job_cache_finish(uint32_t job_id, uint32_t total_size, uint16_t expected_crc)
{
    if (!g_receiving || job_id != g_job_id || total_size != g_total_size) {
        return SLE_JOB_STATUS_BAD_JOB;
    }
    if (g_received != g_total_size) {
        return SLE_JOB_STATUS_BAD_OFFSET;
    }
    if (expected_crc != 0 && expected_crc != g_expected_crc) {
        return SLE_JOB_STATUS_BAD_CRC;
    }
    if (g_expected_crc != 0 && g_running_crc != g_expected_crc) {
        osal_printk("[JOB_CACHE] crc fail job=%u calc=0x%04x expect=0x%04x\r\n",
                    (unsigned int)g_job_id, g_running_crc, g_expected_crc);
        return SLE_JOB_STATUS_BAD_CRC;
    }

    g_receiving = false;
    g_ready = true;
    return SLE_JOB_STATUS_OK;
}

int sle_job_cache_read_byte(void)
{
    if (g_data_len == 0) {
        return -1;
    }

    if (g_mutex_ready) {
        osal_mutex_lock(&g_cache_mutex);
    }

    uint8_t ch = g_cache[g_read_pos];
    g_read_pos = (g_read_pos + 1U) % SLE_JOB_CACHE_SIZE;
    g_data_len--;
    g_consumed++;

    if (g_mutex_ready) {
        osal_mutex_unlock(&g_cache_mutex);
    }
    return (int)ch;
}

uint32_t sle_job_cache_size(void)
{
    return SLE_JOB_CACHE_SIZE;
}

uint32_t sle_job_cache_received(void)
{
    return g_received;
}

uint32_t sle_job_cache_consumed(void)
{
    return g_consumed;
}

uint32_t sle_job_cache_total_size(void)
{
    return g_total_size;
}

uint32_t sle_job_cache_free(void)
{
    return SLE_JOB_CACHE_SIZE - g_data_len;
}

uint32_t sle_job_cache_available(void)
{
    return g_data_len;
}

uint32_t sle_job_cache_job_id(void)
{
    return g_job_id;
}

uint16_t sle_job_cache_crc(void)
{
    return g_running_crc;
}

bool sle_job_cache_is_ready(void)
{
    return g_ready;
}

void sle_job_cache_set_all_received(void)
{
    g_all_received = true;
}

bool sle_job_cache_is_all_received(void)
{
    return g_all_received;
}
