/**
 * @file spi_bus.h
 * @brief SPI bus mutex stub for LCD-only mode (no SD).
 */
#ifndef SPI_BUS_H
#define SPI_BUS_H

#include <stdint.h>
#include <stdbool.h>
#include "errcode.h"

errcode_t spi_bus_init(void);

#endif
