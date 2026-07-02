/**
 * @file sd_spi.h
 * @brief Minimal SPI-mode SD card block reader.
 */
#ifndef SD_SPI_H
#define SD_SPI_H

#include <stdbool.h>
#include <stdint.h>
#include "errcode.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_SPI_SECTOR_SIZE 512U

errcode_t sd_spi_init_card(void);
errcode_t sd_spi_read_sector(uint32_t sector, uint8_t *buf, uint32_t buf_size);
bool sd_spi_is_ready(void);
const char *sd_spi_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
