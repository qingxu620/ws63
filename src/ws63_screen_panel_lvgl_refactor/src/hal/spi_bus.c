/**
 * @file spi_bus.c
 * @brief SPI bus stub - no-op in LCD-only mode.
 */
#include "spi_bus.h"
#include "soc_osal.h"

errcode_t spi_bus_init(void)
{
    return ERRCODE_SUCC;
}
