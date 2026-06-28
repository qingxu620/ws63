/**
 * @file spi_bus.c
 * @brief Shared SPI bus arbitration for LCD and future SD access.
 */
#include "spi_bus.h"

#include "soc_osal.h"

static osal_mutex g_spi_bus_mutex;
static bool g_spi_bus_ready;

errcode_t spi_bus_init(void)
{
    if (g_spi_bus_ready) {
        return ERRCODE_SUCC;
    }

    if (osal_mutex_init(&g_spi_bus_mutex) != OSAL_SUCCESS) {
        osal_printk("[SCREEN] spi bus mutex init failed\r\n");
        return ERRCODE_FAIL;
    }

    g_spi_bus_ready = true;
    return ERRCODE_SUCC;
}

errcode_t spi_bus_lock(uint32_t timeout_ms)
{
    if (!g_spi_bus_ready) {
        return ERRCODE_FAIL;
    }

    if (osal_mutex_lock_timeout(&g_spi_bus_mutex, timeout_ms) != OSAL_SUCCESS) {
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCC;
}

void spi_bus_unlock(void)
{
    if (!g_spi_bus_ready) {
        return;
    }

    osal_mutex_unlock(&g_spi_bus_mutex);
}
