/**
 * @file panel_file_manager.c
 * @brief TF/SD card file manager facade with a fake backend.
 */
#include "panel_file_manager.h"
#include "soc_osal.h"
#include <stdio.h>
#include <string.h>

#ifndef PANEL_FILE_ENABLE_POSIX_BACKEND
#define PANEL_FILE_ENABLE_POSIX_BACKEND 0
#endif

#if PANEL_FILE_ENABLE_POSIX_BACKEND
#include <dirent.h>
#include <sys/stat.h>
#endif

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

#if PANEL_FILE_ENABLE_POSIX_BACKEND
static const char *g_sd_roots[] = {
    "/sd",
    "/sdcard",
    "/mnt/sd",
    "/storage/sd",
};

static bool has_gcode_ext(const char *name)
{
    if (name == NULL) {
        return false;
    }
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return false;
    }
    return strcmp(dot, ".gcode") == 0 || strcmp(dot, ".GCODE") == 0 ||
           strcmp(dot, ".nc") == 0 || strcmp(dot, ".NC") == 0 ||
           strcmp(dot, ".gco") == 0 || strcmp(dot, ".GCO") == 0;
}

static panel_file_type_t type_from_name(const char *name)
{
    if (has_gcode_ext(name)) {
        return PANEL_FILE_TYPE_GCODE;
    }
    return PANEL_FILE_TYPE_TEXT;
}

static uint32_t count_file_lines(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }

    uint32_t lines = 0;
    int ch;
    bool saw_any = false;
    while ((ch = fgetc(fp)) != EOF) {
        saw_any = true;
        if (ch == '\n') {
            lines++;
        }
    }
    fclose(fp);
    return (saw_any && lines == 0U) ? 1U : lines;
}

static bool add_real_entry(const char *root, const char *name, uint32_t size_bytes)
{
    if (root == NULL || name == NULL || g_file_mgr.count >= PANEL_FILE_MAX_COUNT) {
        return false;
    }

    panel_file_type_t type = type_from_name(name);
    panel_file_entry_t *dst = &g_file_mgr.entries[g_file_mgr.count];
    snprintf(dst->name, sizeof(dst->name), "%s", name);
    snprintf(dst->path, sizeof(dst->path), "%s/%s", root, name);
    dst->size_bytes = size_bytes;
    dst->line_count = is_gcode_type(type) ? count_file_lines(dst->path) : 0U;
    dst->type = type;
    dst->selectable = is_gcode_type(type);
    g_file_mgr.count++;
    return true;
}

static bool try_refresh_real_backend(void)
{
    for (uint8_t r = 0; r < (uint8_t)(sizeof(g_sd_roots) / sizeof(g_sd_roots[0])); r++) {
        const char *root = g_sd_roots[r];
        DIR *dir = opendir(root);
        if (dir == NULL) {
            continue;
        }

        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && g_file_mgr.count < PANEL_FILE_MAX_COUNT) {
            if (ent->d_name[0] == '.') {
                continue;
            }

            char path[PANEL_FILE_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", root, ent->d_name);
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }
            (void)add_real_entry(root, ent->d_name, (uint32_t)st.st_size);
        }
        closedir(dir);

        g_file_mgr.mounted = true;
        g_file_mgr.real_backend = true;
        snprintf(g_file_mgr.mount_label, sizeof(g_file_mgr.mount_label), "%s", root);
        set_error((g_file_mgr.count > 0U) ? "无" : "TF目录为空");
        return true;
    }
    return false;
}
#endif

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
    g_file_mgr.mounted = false;
    g_file_mgr.real_backend = false;
    g_file_mgr.count = 0;
    g_file_mgr.seq++;
    g_file_mgr.selected_index = -1;
    set_error("未发现TF挂载点");

#if PANEL_FILE_ENABLE_POSIX_BACKEND
    if (try_refresh_real_backend()) {
        return;
    }
#endif

    g_file_mgr.mounted = true;
    snprintf(g_file_mgr.mount_label, sizeof(g_file_mgr.mount_label), "TF");
    set_error("真实TF后端未接入，使用示例数据");

    uint8_t limit = (uint8_t)(sizeof(g_fake_files) / sizeof(g_fake_files[0]));
    if (limit > PANEL_FILE_MAX_COUNT) {
        limit = PANEL_FILE_MAX_COUNT;
    }

    for (uint8_t i = 0; i < limit; i++) {
        panel_file_entry_t *dst = &g_file_mgr.entries[i];
        snprintf(dst->name, sizeof(dst->name), "%s", g_fake_files[i].name);
        snprintf(dst->path, sizeof(dst->path), "fake:%s", g_fake_files[i].name);
        dst->size_bytes = g_fake_files[i].size_bytes;
        dst->line_count = g_fake_files[i].line_count;
        dst->type = g_fake_files[i].type;
        dst->selectable = is_gcode_type(dst->type);
        g_file_mgr.count++;
    }

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

#if PANEL_FILE_ENABLE_POSIX_BACKEND
    if (g_file_mgr.real_backend) {
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
#endif

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

#if PANEL_FILE_ENABLE_POSIX_BACKEND
    if (g_file_mgr.real_backend) {
        FILE *fp = fopen(entry->path, "rb");
        if (fp == NULL) {
            set_error("打开TF文件失败");
            return false;
        }
        if (fseek(fp, (long)offset, SEEK_SET) != 0) {
            fclose(fp);
            set_error("定位TF文件失败");
            return false;
        }
        uint32_t remain = entry->size_bytes - offset;
        size_t n = out_size;
        if ((uint32_t)n > remain) {
            n = (size_t)remain;
        }
        size_t got = fread(out, 1, n, fp);
        fclose(fp);
        if (bytes_read != NULL) {
            *bytes_read = got;
        }
        if (eof != NULL) {
            *eof = (offset + (uint32_t)got) >= entry->size_bytes;
        }
        return got > 0U || remain == 0U;
    }
#endif

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
