/**
 * @file spi_bus.c
 * @brief SPI bus stub - no-op in LCD-only mode.
 */
#include "spi_bus.h"
#include "soc_osal.h"

errcode_t spi_bus_init(void)
{
    osal_printk("[SPI_BUS] init (lcd-only mode)\r\n");
    return ERRCODE_SUCC;
}
