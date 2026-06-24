/**
 * @file panel_file_manager.h
 * @brief TF/SD card file manager facade for the panel UI.
 *
 * Current backend is a deterministic fake list. Real TF card support should
 * replace only this service layer after SPI-SD/FATFS is validated.
 */
#ifndef PANEL_FILE_MANAGER_H
#define PANEL_FILE_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_FILE_MAX_COUNT      6
#define PANEL_FILE_NAME_MAX       32
#define PANEL_FILE_PREVIEW_MAX    96

typedef enum {
    PANEL_FILE_TYPE_GCODE = 0,
    PANEL_FILE_TYPE_TEXT,
    PANEL_FILE_TYPE_DIR,
    PANEL_FILE_TYPE_UNKNOWN
} panel_file_type_t;

typedef struct {
    char name[PANEL_FILE_NAME_MAX];
    uint32_t size_bytes;
    uint32_t line_count;
    panel_file_type_t type;
    bool selectable;
} panel_file_entry_t;

typedef struct {
    bool mounted;
    bool real_backend;
    uint32_t seq;
    uint8_t count;
    int8_t selected_index;
    char mount_label[16];
    char last_error[32];
    panel_file_entry_t entries[PANEL_FILE_MAX_COUNT];
} panel_file_manager_t;

void panel_file_manager_init(void);
void panel_file_manager_refresh(void);

const panel_file_manager_t *panel_file_manager_get(void);
const panel_file_entry_t *panel_file_manager_get_entry(uint8_t index);
const panel_file_entry_t *panel_file_manager_get_selected(void);

bool panel_file_manager_select(uint8_t index);
bool panel_file_manager_read_preview(uint8_t index, char *out, size_t out_size);
bool panel_file_manager_read_chunk(uint8_t index, uint32_t offset, uint8_t *out,
                                   size_t out_size, size_t *bytes_read, bool *eof);

const char *panel_file_manager_type_text(panel_file_type_t type);

#ifdef __cplusplus
}
#endif

#endif
