/**
 * @file panel_file_manager.c
 * @brief TF/SD card file manager facade with a fake backend.
 */
#include "panel_file_manager.h"
#include "soc_osal.h"
#include <stdio.h>
#include <string.h>

static panel_file_manager_t g_file_mgr;

typedef struct {
    const char *name;
    uint32_t size_bytes;
    uint32_t line_count;
    panel_file_type_t type;
    const char *preview;
} fake_file_t;

static const fake_file_t g_fake_files[] = {
    {
        "demo_box_20x20.gcode",
        21322U,
        328U,
        PANEL_FILE_TYPE_GCODE,
        "G21\nG90\nM3 S10\nG1 X20 Y0 F1000\nG1 X20 Y20\nM5\n"
    },
    {
        "focus_grid_s10.nc",
        4820U,
        96U,
        PANEL_FILE_TYPE_GCODE,
        "G21\nG90\nM3 S10\nG1 X0 Y0 F800\nG1 X10 Y0\nM5\n"
    },
    {
        "nameplate_test.gco",
        35680U,
        612U,
        PANEL_FILE_TYPE_GCODE,
        "G21\nG90\nG0 X0 Y0\nM3 S120\nG1 X40 Y0 F1200\nM5\n"
    },
    {
        "readme.txt",
        640U,
        18U,
        PANEL_FILE_TYPE_TEXT,
        "Place .gcode/.nc/.gco files in TF root directory.\n"
    },
};

static void set_error(const char *text)
{
    if (text == NULL) {
        text = "无";
    }
    snprintf(g_file_mgr.last_error, sizeof(g_file_mgr.last_error), "%s", text);
}

static bool is_gcode_type(panel_file_type_t type)
{
    return type == PANEL_FILE_TYPE_GCODE;
}

void panel_file_manager_init(void)
{
    memset(&g_file_mgr, 0, sizeof(g_file_mgr));
    snprintf(g_file_mgr.mount_label, sizeof(g_file_mgr.mount_label), "TF");
    g_file_mgr.selected_index = -1;
    set_error("无");
    panel_file_manager_refresh();
}

void panel_file_manager_refresh(void)
{
    memset(g_file_mgr.entries, 0, sizeof(g_file_mgr.entries));
    g_file_mgr.mounted = true;
    g_file_mgr.real_backend = false;
    g_file_mgr.count = 0;
    g_file_mgr.seq++;
    g_file_mgr.selected_index = -1;
    set_error("真实TF后端未接入");

    uint8_t limit = (uint8_t)(sizeof(g_fake_files) / sizeof(g_fake_files[0]));
    if (limit > PANEL_FILE_MAX_COUNT) {
        limit = PANEL_FILE_MAX_COUNT;
    }

    for (uint8_t i = 0; i < limit; i++) {
        panel_file_entry_t *dst = &g_file_mgr.entries[i];
        snprintf(dst->name, sizeof(dst->name), "%s", g_fake_files[i].name);
        dst->size_bytes = g_fake_files[i].size_bytes;
        dst->line_count = g_fake_files[i].line_count;
        dst->type = g_fake_files[i].type;
        dst->selectable = is_gcode_type(dst->type);
        g_file_mgr.count++;
    }

    osal_printk("[FILE_MGR] refresh fake TF list count=%u seq=%u\r\n",
                (unsigned int)g_file_mgr.count,
                (unsigned int)g_file_mgr.seq);
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
        osal_printk("[FILE_MGR] select rejected index=%u\r\n", (unsigned int)index);
        return false;
    }

    g_file_mgr.selected_index = (int8_t)index;
    g_file_mgr.seq++;
    osal_printk("[FILE_MGR] selected %s size=%u lines=%u\r\n",
                entry->name,
                (unsigned int)entry->size_bytes,
                (unsigned int)entry->line_count);
    return true;
}

bool panel_file_manager_read_preview(uint8_t index, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    if (index >= g_file_mgr.count) {
        return false;
    }

    const char *preview = g_fake_files[index].preview;
    if (preview == NULL) {
        preview = "";
    }
    snprintf(out, out_size, "%s", preview);
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
    if (entry == NULL || out == NULL || out_size == 0 || !entry->selectable) {
        return false;
    }
    if (offset >= entry->size_bytes) {
        return true;
    }

    const char *preview = g_fake_files[index].preview;
    size_t preview_len = strlen(preview);
    if (preview_len == 0) {
        return false;
    }

    uint32_t remain = entry->size_bytes - offset;
    size_t n = out_size;
    if ((uint32_t)n > remain) {
        n = (size_t)remain;
    }

    for (size_t i = 0; i < n; i++) {
        out[i] = (uint8_t)preview[(offset + (uint32_t)i) % preview_len];
    }

    if (bytes_read != NULL) {
        *bytes_read = n;
    }
    if (eof != NULL) {
        *eof = (offset + (uint32_t)n) >= entry->size_bytes;
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
