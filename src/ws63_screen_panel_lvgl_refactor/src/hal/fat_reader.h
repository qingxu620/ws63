/**
 * @file fat_reader.h
 * @brief Minimal FAT16/FAT32 root-directory reader for G-code files.
 */
#ifndef FAT_READER_H
#define FAT_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "errcode.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAT_READER_MAX_FILES 8U
#define FAT_READER_NAME_MAX  64U

typedef struct {
    char name[FAT_READER_NAME_MAX];
    uint32_t size_bytes;
    uint32_t first_cluster;
} fat_reader_file_t;

errcode_t fat_reader_mount(void);
uint8_t fat_reader_file_count(void);
const fat_reader_file_t *fat_reader_get_file(uint8_t index);
errcode_t fat_reader_read_file(uint8_t index, uint32_t offset, uint8_t *out,
                               size_t out_size, size_t *bytes_read, bool *eof);
const char *fat_reader_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
