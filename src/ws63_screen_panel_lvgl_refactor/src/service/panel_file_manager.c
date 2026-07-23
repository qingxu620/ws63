/**
 * @file panel_file_manager.c
 * @brief SD/FAT G-code file manager facade for the panel UI.
 */
#include "panel_file_manager.h"
#include "fat_reader.h"
#include "panel_offline_job.h"
#include "task_manager.h"

#include "soc_osal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define PANEL_FILE_LINE_SCAN_CHUNK 512U
#define PANEL_FILE_SCAN_TASK_STACK_SIZE 0x1000U
#define PANEL_FILE_SCAN_TASK_PRIORITY   27U

static panel_file_manager_t g_file_mgr;
static bool g_file_mgr_initialized;
static osal_semaphore g_scan_sem;
static osal_mutex g_file_io_mutex;
static bool g_scan_sem_ready;
static bool g_file_io_mutex_ready;
static volatile bool g_scan_worker_created;
static volatile bool g_scan_requested;
static char g_refresh_selected_name[PANEL_FILE_NAME_MAX];
static int8_t g_refresh_selected_index = -1;

static void set_manager_error(panel_file_manager_t *mgr, const char *text)
{
    if (mgr == NULL) {
        return;
    }
    if (text == NULL) {
        text = "无";
    }
    snprintf(mgr->last_error, sizeof(mgr->last_error), "%s", text);
}

static void set_error(const char *text)
{
    set_manager_error(&g_file_mgr, text);
}

static void scan_sd_files(panel_file_manager_t *result, const char *selected_name)
{
    memset(result, 0, sizeof(*result));
    snprintf(result->mount_label, sizeof(result->mount_label), "SD");
    result->selected_index = -1;

    osal_printk("[PANEL_FILE] SD scan start\r\n");
    errcode_t ret = fat_reader_mount();
    if (ret != ERRCODE_SUCC) {
        set_manager_error(result, fat_reader_last_error());
        osal_printk("[PANEL_FILE] SD mount failed: %s\r\n", result->last_error);
        return;
    }

    result->mounted = true;
    result->real_backend = true;

    uint8_t limit = fat_reader_file_count();
    if (limit > PANEL_FILE_MAX_COUNT) {
        limit = PANEL_FILE_MAX_COUNT;
    }

    for (uint8_t i = 0; i < limit; i++) {
        const fat_reader_file_t *src = fat_reader_get_file(i);
        if (src == NULL) {
            continue;
        }
        panel_file_entry_t *dst = &result->entries[result->count];
        snprintf(dst->name, sizeof(dst->name), "%s", src->name);
        snprintf(dst->path, sizeof(dst->path), "sd:/%s", src->name);
        dst->size_bytes = src->size_bytes;
        dst->line_count = 0;
        dst->type = PANEL_FILE_TYPE_GCODE;
        dst->selectable = true;
        osal_printk("[PANEL_FILE] file[%u] name=%s size=%lu lines=%lu\r\n",
                    (unsigned int)result->count,
                    dst->name,
                    (unsigned long)dst->size_bytes,
                    (unsigned long)dst->line_count);
        if (selected_name != NULL && selected_name[0] != '\0' &&
            strcmp(selected_name, dst->name) == 0) {
            result->selected_index = (int8_t)result->count;
        }
        result->count++;
    }

    set_manager_error(result, (result->count > 0U) ? "无" : "SD卡无G-code文件");
    osal_printk("[PANEL_FILE] SD scan done mounted=%u files=%u err=%s\r\n",
                result->mounted ? 1U : 0U,
                (unsigned int)result->count,
                result->last_error);
}

static int panel_file_scan_task(void *arg)
{
    (void)arg;
    osal_printk("[PANEL_FILE_TASK] ready stack=0x%x priority=%u\r\n",
                PANEL_FILE_SCAN_TASK_STACK_SIZE, PANEL_FILE_SCAN_TASK_PRIORITY);

    while (1) {
        if (osal_sem_down(&g_scan_sem) != OSAL_SUCCESS) {
            osal_printk("[PANEL_FILE_TASK] scan semaphore wait failed\r\n");
            osal_msleep(20);
            continue;
        }
        if (!g_scan_requested) {
            continue;
        }

        /* A queued offline job owns the FAT reader until it finishes. */
        while (panel_offline_job_is_busy()) {
            osal_msleep(20);
        }

        if (osal_mutex_lock(&g_file_io_mutex) != OSAL_SUCCESS) {
            uint32_t lock = osal_irq_lock();
            g_file_mgr.scanning = false;
            g_file_mgr.selected_index = g_refresh_selected_index;
            g_file_mgr.seq++;
            g_scan_requested = false;
            osal_irq_restore(lock);
            set_error("SD扫描锁获取失败");
            osal_printk("[PANEL_FILE_TASK] file I/O lock failed\r\n");
            continue;
        }

        /* Keep the staging snapshot on this task's 4 KiB stack, not static DTCM. */
        panel_file_manager_t scan_result;
        scan_sd_files(&scan_result, g_refresh_selected_name);
        osal_mutex_unlock(&g_file_io_mutex);

        /* Publish one complete snapshot; never expose a half-filled file list. */
        uint32_t lock = osal_irq_lock();
        uint32_t next_seq = g_file_mgr.seq + 1U;
        memcpy(&g_file_mgr, &scan_result, sizeof(g_file_mgr));
        g_file_mgr.seq = next_seq;
        g_file_mgr.scanning = false;
        g_scan_requested = false;
        osal_irq_restore(lock);
    }
    return 0;
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
    set_error("等待启动扫描");
    g_file_mgr_initialized = true;

    if (osal_sem_init(&g_scan_sem, 0) != OSAL_SUCCESS) {
        set_error("SD扫描信号量初始化失败");
        osal_printk("[PANEL_FILE] scan semaphore init failed\r\n");
        return;
    }
    g_scan_sem_ready = true;

    if (osal_mutex_init(&g_file_io_mutex) != OSAL_SUCCESS) {
        set_error("SD文件锁初始化失败");
        osal_printk("[PANEL_FILE] file I/O mutex init failed\r\n");
        return;
    }
    g_file_io_mutex_ready = true;

    errcode_t ret = task_create("panel_file_scan", panel_file_scan_task, NULL,
                                PANEL_FILE_SCAN_TASK_STACK_SIZE,
                                PANEL_FILE_SCAN_TASK_PRIORITY);
    if (ret != ERRCODE_SUCC) {
        set_error("SD扫描任务创建失败");
        osal_printk("[PANEL_FILE] scan task create failed: 0x%x\r\n", ret);
        return;
    }
    g_scan_worker_created = true;
}

errcode_t panel_file_manager_refresh(void)
{
    if (!g_file_mgr_initialized || !g_scan_sem_ready ||
        !g_file_io_mutex_ready || !g_scan_worker_created) {
        set_error("SD扫描任务未就绪");
        return ERRCODE_FAIL;
    }
    if (panel_offline_job_is_busy()) {
        set_error("离线任务执行中，暂不刷新");
        g_file_mgr.seq++;
        return ERRCODE_FAIL;
    }

    char selected_name[PANEL_FILE_NAME_MAX] = {0};
    int8_t selected_index = g_file_mgr.selected_index;
    if (selected_index >= 0 && (uint8_t)selected_index < g_file_mgr.count) {
        snprintf(selected_name, sizeof(selected_name), "%s",
                 g_file_mgr.entries[(uint8_t)selected_index].name);
    }

    uint32_t lock = osal_irq_lock();
    if (g_scan_requested || g_file_mgr.scanning) {
        osal_irq_restore(lock);
        return ERRCODE_SUCC;
    }

    memcpy(g_refresh_selected_name, selected_name, sizeof(g_refresh_selected_name));
    g_refresh_selected_index = selected_index;
    g_file_mgr.selected_index = -1;
    g_file_mgr.scanning = true;
    g_file_mgr.seq++;
    g_scan_requested = true;
    osal_irq_restore(lock);

    set_error("正在扫描SD卡");
    osal_sem_up(&g_scan_sem);
    return ERRCODE_SUCC;
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
    if (g_file_mgr.scanning || g_file_mgr.selected_index < 0) {
        return NULL;
    }
    return panel_file_manager_get_entry((uint8_t)g_file_mgr.selected_index);
}

bool panel_file_manager_select(uint8_t index)
{
    if (g_file_mgr.scanning || panel_offline_job_is_busy()) {
        return false;
    }
    const panel_file_entry_t *entry = panel_file_manager_get_entry(index);
    if (entry == NULL || !entry->selectable) {
        return false;
    }

    g_file_mgr.selected_index = (int8_t)index;
    g_file_mgr.seq++;
    return true;
}

bool panel_file_manager_ensure_line_count(uint8_t index, uint32_t *line_count)
{
    if (line_count != NULL) {
        *line_count = 0U;
    }
    if (g_file_mgr.scanning || index >= g_file_mgr.count) {
        set_error("文件索引无效");
        return false;
    }

    panel_file_entry_t *entry = &g_file_mgr.entries[index];
    if (!entry->selectable || entry->size_bytes == 0U) {
        set_error("文件大小无效");
        return false;
    }
    if (entry->line_count > 0U) {
        if (line_count != NULL) {
            *line_count = entry->line_count;
        }
        return true;
    }

    uint8_t buf[PANEL_FILE_LINE_SCAN_CHUNK];
    uint32_t offset = 0U;
    uint32_t count = 0U;
    bool line_has_code = false;
    bool in_semicolon_comment = false;

    while (offset < entry->size_bytes) {
        size_t bytes_read = 0U;
        bool eof = false;
        if (!panel_file_manager_read_chunk(index, offset, buf, sizeof(buf),
                                           &bytes_read, &eof) || bytes_read == 0U) {
            set_error("统计G-code行数失败");
            return false;
        }

        for (size_t i = 0U; i < bytes_read; i++) {
            uint8_t ch = buf[i];
            if (ch == '\r' || ch == '\n') {
                if (line_has_code) {
                    count++;
                }
                line_has_code = false;
                in_semicolon_comment = false;
                continue;
            }
            if (in_semicolon_comment) {
                continue;
            }
            if (ch == ';') {
                in_semicolon_comment = true;
                continue;
            }
            if (!isspace((int)ch)) {
                line_has_code = true;
            }
        }

        offset += (uint32_t)bytes_read;
        if (eof) {
            break;
        }
    }

    if (line_has_code) {
        count++;
    }
    entry->line_count = count;
    g_file_mgr.seq++;
    if (line_count != NULL) {
        *line_count = count;
    }
    return true;
}

bool panel_file_manager_read_preview(uint8_t index, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return false;
    }
    out[0] = '\0';

    /* Preview is UI-only; the offline worker has exclusive ownership while busy. */
    if (g_file_mgr.scanning || panel_offline_job_is_busy() ||
        index >= g_file_mgr.count) {
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
    if (g_file_mgr.scanning || !g_file_io_mutex_ready || entry == NULL ||
        out == NULL || out_size == 0U || !entry->selectable) {
        return false;
    }

    if (osal_mutex_lock(&g_file_io_mutex) != OSAL_SUCCESS) {
        set_error("SD文件锁获取失败");
        return false;
    }
    errcode_t ret = fat_reader_read_file(index, offset, out, out_size, bytes_read, eof);
    if (ret != ERRCODE_SUCC) {
        set_error(fat_reader_last_error());
        osal_mutex_unlock(&g_file_io_mutex);
        return false;
    }
    osal_mutex_unlock(&g_file_io_mutex);
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
