/**
 * @file spi_bus.h
 * @brief SPI bus arbitration for LCD access.
 */
#ifndef SPI_BUS_H
#define SPI_BUS_H

#include <stdint.h>
#include <stdbool.h>
#include "errcode.h"

errcode_t spi_bus_init(void);
errcode_t spi_bus_lock(uint32_t timeout_ms);
void spi_bus_unlock(void);

void spi_bus_park_pins_for_boot(void);

errcode_t spi_bus_enter_sd_mode(void);
errcode_t spi_bus_enter_sd_fast(void);
errcode_t spi_bus_restore_lcd_mode(void);

void spi_bus_sd_cs_low(void);
void spi_bus_sd_cs_high(void);
void spi_bus_lcd_cs_high(void);

#endif
