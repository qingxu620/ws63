/**
 * @file panel_file_manager.c
 * @brief SD/FAT G-code file manager facade for the panel UI.
 */
#include "panel_file_manager.h"
#include "fat_reader.h"

#include "soc_osal.h"

#include <stdio.h>
#include <string.h>

static panel_file_manager_t g_file_mgr;
static bool g_file_mgr_initialized;

static void set_error(const char *text)
{
    if (text == NULL) {
        text = "无";
    }
    snprintf(g_file_mgr.last_error, sizeof(g_file_mgr.last_error), "%s", text);
}

void panel_file_manager_init(void)
{
    if (g_file_mgr_initialized) {
        return;
    }

    memset(&g_file_mgr, 0, sizeof(g_file_mgr));
    snprintf(g_file_mgr.mount_label, sizeof(g_file_mgr.mount_label), "SD");
    g_file_mgr.selected_index = -1;
    g_file_mgr.seq++;
    set_error("未挂载，进入SD任务页后点刷新");
    g_file_mgr_initialized = true;
}

void panel_file_manager_refresh(void)
{
    osal_printk("[PANEL_FILE] SD scan start\r\n");

    char selected_name[PANEL_FILE_NAME_MAX] = {0};
    const panel_file_entry_t *selected = panel_file_manager_get_selected();
    if (selected != NULL) {
        snprintf(selected_name, sizeof(selected_name), "%s", selected->name);
    }

    memset(g_file_mgr.entries, 0, sizeof(g_file_mgr.entries));
    g_file_mgr.count = 0;
    g_file_mgr.seq++;
    g_file_mgr.selected_index = -1;
    snprintf(g_file_mgr.mount_label, sizeof(g_file_mgr.mount_label), "SD");

    errcode_t ret = fat_reader_mount();
    if (ret != ERRCODE_SUCC) {
        g_file_mgr.mounted = false;
        g_file_mgr.real_backend = false;
        set_error(fat_reader_last_error());
        osal_printk("[PANEL_FILE] SD mount failed: %s\r\n", g_file_mgr.last_error);
        return;
    }

    g_file_mgr.mounted = true;
    g_file_mgr.real_backend = true;

    uint8_t limit = fat_reader_file_count();
    if (limit > PANEL_FILE_MAX_COUNT) {
        limit = PANEL_FILE_MAX_COUNT;
    }

    for (uint8_t i = 0; i < limit; i++) {
        const fat_reader_file_t *src = fat_reader_get_file(i);
        if (src == NULL) {
            continue;
        }
        panel_file_entry_t *dst = &g_file_mgr.entries[g_file_mgr.count];
        snprintf(dst->name, sizeof(dst->name), "%s", src->name);
        snprintf(dst->path, sizeof(dst->path), "sd:/%s", src->name);
        dst->size_bytes = src->size_bytes;
        dst->line_count = 0;
        dst->type = PANEL_FILE_TYPE_GCODE;
        dst->selectable = true;
        osal_printk("[PANEL_FILE] file[%u] name=%s size=%lu lines=%lu\r\n",
                    (unsigned int)g_file_mgr.count,
                    dst->name,
                    (unsigned long)dst->size_bytes,
                    (unsigned long)dst->line_count);
        if (selected_name[0] != '\0' && strcmp(selected_name, dst->name) == 0) {
            g_file_mgr.selected_index = (int8_t)g_file_mgr.count;
        }
        g_file_mgr.count++;
    }

    set_error((g_file_mgr.count > 0U) ? "无" : "SD卡无G-code文件");
    osal_printk("[PANEL_FILE] SD scan done mounted=%u files=%u err=%s\r\n",
                g_file_mgr.mounted ? 1U : 0U,
                (unsigned int)g_file_mgr.count,
                g_file_mgr.last_error);
}

const panel_file_manager_t *panel_file_manager_get(void)
{
    return &g_file_mgr;
}

const panel_file_entry_t *panel_file_manager_get_entry(uint8_t index)
{
    if (index >= g_file_mgr.count) {
        return NULL;
    }
    return &g_file_mgr.entries[index];
}

const panel_file_entry_t *panel_file_manager_get_selected(void)
{
    if (g_file_mgr.selected_index < 0) {
        return NULL;
    }
    return panel_file_manager_get_entry((uint8_t)g_file_mgr.selected_index);
}

bool panel_file_manager_select(uint8_t index)
{
    const panel_file_entry_t *entry = panel_file_manager_get_entry(index);
    if (entry == NULL || !entry->selectable) {
        return false;
    }

    g_file_mgr.selected_index = (int8_t)index;
    g_file_mgr.seq++;
    return true;
}

bool panel_file_manager_read_preview(uint8_t index, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return false;
    }
    out[0] = '\0';

    if (index >= g_file_mgr.count) {
        return false;
    }

    size_t bytes_read = 0;
    bool eof = false;
    if (!panel_file_manager_read_chunk(index, 0, (uint8_t *)out,
                                       out_size - 1U, &bytes_read, &eof)) {
        return false;
    }
    (void)eof;
    out[bytes_read] = '\0';
    return true;
}

bool panel_file_manager_read_chunk(uint8_t index, uint32_t offset, uint8_t *out,
                                   size_t out_size, size_t *bytes_read, bool *eof)
{
    const panel_file_entry_t *entry = panel_file_manager_get_entry(index);
    if (bytes_read != NULL) {
        *bytes_read = 0;
    }
    if (eof != NULL) {
        *eof = true;
    }
    if (entry == NULL || out == NULL || out_size == 0U || !entry->selectable) {
        return false;
    }

    if (fat_reader_read_file(index, offset, out, out_size, bytes_read, eof) != ERRCODE_SUCC) {
        set_error(fat_reader_last_error());
        return false;
    }
    return true;
}

const char *panel_file_manager_type_text(panel_file_type_t type)
{
    switch (type) {
    case PANEL_FILE_TYPE_GCODE: return "G-code";
    case PANEL_FILE_TYPE_TEXT: return "文本";
    case PANEL_FILE_TYPE_DIR: return "目录";
    default: return "未知";
    }
}
