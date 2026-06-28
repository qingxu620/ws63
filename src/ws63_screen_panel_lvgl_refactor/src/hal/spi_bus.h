/**
 * @file spi_bus.h
 * @brief Shared SPI bus arbitration for LCD and future SD access.
 */
#ifndef SPI_BUS_H
#define SPI_BUS_H

#include <stdint.h>
#include <stdbool.h>
#include "errcode.h"

errcode_t spi_bus_init(void);
errcode_t spi_bus_lock(uint32_t timeout_ms);
void spi_bus_unlock(void);

#endif
