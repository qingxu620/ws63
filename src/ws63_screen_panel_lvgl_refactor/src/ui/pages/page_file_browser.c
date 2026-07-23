/**
 * @file page_file_browser.c
 * @brief Offline SD file browser page.
 */
#include "page_file_browser.h"
#include "panel_theme.h"
#include "ui_manager.h"
#include "../service/panel_file_manager.h"
#include "../service/panel_model.h"
#include "../service/panel_offline_job.h"
#include "../service/panel_transport_sle.h"
#include "soc_osal.h"
#include <stdint.h>
#include <stdio.h>

typedef struct {
    lv_obj_t *btn;
    lv_obj_t *name;
    lv_obj_t *meta;
} file_row_t;

static lv_obj_t *g_lbl_status;
static lv_obj_t *g_lbl_selected;
static lv_obj_t *g_lbl_preview;
static lv_obj_t *g_btn_start;
static lv_obj_t *g_btn_monitor;
static lv_obj_t *g_lbl_start;
static lv_obj_t *g_lbl_monitor;
static file_row_t g_rows[PANEL_FILE_MAX_COUNT];
static uint32_t g_rendered_seq = UINT32_MAX;
static int8_t g_rendered_selected_index = INT8_MIN;
static bool g_rendered_busy = false;
static bool g_rendered_tx_present = false;
static bool g_rendered_rx_ready = false;
static bool g_rendered_worker_ready = false;
static bool g_rendered_rx_claimed = false;

static void bind_click(lv_obj_t *obj, lv_event_cb_t cb, void *user_data)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(obj, 6);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, user_data);
}

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    osal_printk("[FILE_PAGE] back -> home\r\n");
    ui_manager_switch_page(PAGE_HOME);
}

static void refresh_btn_cb(lv_event_t *e)
{
    (void)e;
    if (panel_offline_job_is_busy()) {
        osal_printk("[FILE_PAGE] refresh rejected: offline job busy\r\n");
        return;
    }
    osal_printk("[FILE_PAGE] refresh click\r\n");
    errcode_t ret = panel_file_manager_refresh();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[FILE_PAGE] refresh queue failed: 0x%x\r\n", ret);
    }
    page_file_browser_update();
}

static void online_btn_cb(lv_event_t *e)
{
    (void)e;
    panel_model_t model;
    panel_model_get_snapshot(&model);
    if (model.view_mode == PANEL_VIEW_OFFLINE) {
        panel_model_toggle_primary_mode();
        panel_model_get_snapshot(&model);
    }
    if (model.view_mode == PANEL_VIEW_ONLINE) {
        osal_printk("[FILE_PAGE] return online requested\r\n");
        ui_manager_switch_page(PAGE_HOME);
    } else {
        osal_printk("[FILE_PAGE] return online rejected state=%u owner=%u\r\n",
                    (unsigned int)model.state, (unsigned int)model.owner);
    }
}

static void file_row_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    const panel_file_manager_t *mgr = panel_file_manager_get();
    if (mgr->scanning) {
        osal_printk("[FILE_PAGE] select rejected: SD scan active\r\n");
        return;
    }
    const panel_file_entry_t *entry = panel_file_manager_get_entry(index);
    if (entry == NULL) {
        osal_printk("[FILE_PAGE] click invalid index=%u\r\n", (unsigned int)index);
        return;
    }

    if (panel_offline_job_is_busy() || panel_transport_sle_tx_is_connected()) {
        osal_printk("[FILE_PAGE] select rejected: busy=%d tx=%d\r\n",
                    panel_offline_job_is_busy(), panel_transport_sle_tx_is_connected());
        return;
    }

    osal_printk("[FILE_PAGE] click file index=%u name=%s size=%lu\r\n",
                (unsigned int)index, entry->name, (unsigned long)entry->size_bytes);

    if (!panel_file_manager_select(index)) {
        osal_printk("[FILE_PAGE] reject non-job file index=%u name=%s\r\n",
                    (unsigned int)index, entry->name);
        return;
    }

    panel_model_select_offline_file(entry->name, entry->size_bytes, entry->line_count);
    osal_printk("[FILE_PAGE] selected file index=%u name=%s\r\n",
                (unsigned int)index, entry->name);
    page_file_browser_update();
}

static void start_btn_cb(lv_event_t *e)
{
    (void)e;
    if (panel_file_manager_get_selected() == NULL) {
        osal_printk("[FILE_PAGE] start rejected: no selected file\r\n");
        return;
    }
    if (!panel_transport_sle_can_control_rx()) {
        osal_printk("[FILE_PAGE] start rejected: RX control not claimed\r\n");
        return;
    }
    if (!panel_transport_sle_rx_is_connected()) {
        osal_printk("[FILE_PAGE] start rejected: RX not ready\r\n");
        return;
    }
    if (!panel_offline_job_is_ready()) {
        osal_printk("[FILE_PAGE] start rejected: offline worker not ready\r\n");
        return;
    }

    osal_printk("[FILE_PAGE] start selected offline job\r\n");
    if (panel_offline_job_start_selected() != ERRCODE_SUCC) {
        osal_printk("[FILE_PAGE] start rejected: offline job busy or invalid\r\n");
        return;
    }
    osal_printk("[FILE_PAGE] offline job queued\r\n");
}

static void monitor_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_switch_page(PAGE_JOB_MONITOR);
}

static lv_obj_t *create_header_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 54, 28);
    lv_obj_set_style_bg_color(btn, COLOR_BG_CARD, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_color(btn, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    bind_click(btn, cb, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(lbl, COLOR_LASER_BLUE, 0);
    lv_obj_center(lbl);
    bind_click(lbl, cb, NULL);
    return btn;
}

static lv_obj_t *create_action_btn(lv_obj_t *parent, const char *text, lv_color_t color,
                                   lv_event_cb_t cb, lv_obj_t **out_label)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 132, 30);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    bind_click(btn, cb, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
    bind_click(lbl, cb, NULL);
    if (out_label != NULL) {
        *out_label = lbl;
    }
    return btn;
}

static void create_file_row(lv_obj_t *parent, uint8_t index)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 290, 42);
    lv_obj_set_style_bg_color(btn, COLOR_BG_CARD, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_color(btn, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_pad_hor(btn, 8, 0);
    lv_obj_set_style_pad_ver(btn, 4, 0);
    lv_obj_set_ext_click_area(btn, 3);
    lv_obj_add_event_cb(btn, file_row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);

    lv_obj_t *col = lv_obj_create(btn);
    lv_obj_set_size(col, 270, 32);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_gap(col, 2, 0);
    lv_obj_add_event_cb(col, file_row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);

    g_rows[index].btn = btn;
    g_rows[index].name = lv_label_create(col);
    lv_label_set_text(g_rows[index].name, "--");
    lv_obj_set_style_text_font(g_rows[index].name, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(g_rows[index].name, COLOR_TEXT_BRIGHT, 0);
    lv_obj_set_width(g_rows[index].name, 268);
    lv_label_set_long_mode(g_rows[index].name, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(g_rows[index].name, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_rows[index].name, file_row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);

    g_rows[index].meta = lv_label_create(col);
    lv_label_set_text(g_rows[index].meta, "--");
    lv_obj_set_style_text_font(g_rows[index].meta, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(g_rows[index].meta, COLOR_TEXT_MUTED, 0);
    lv_obj_add_flag(g_rows[index].meta, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_rows[index].meta, file_row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
}

void page_file_browser_create(lv_obj_t *parent)
{
    g_rendered_seq = UINT32_MAX;
    g_rendered_selected_index = INT8_MIN;
    g_rendered_busy = false;
    g_rendered_tx_present = false;
    g_rendered_rx_ready = false;
    g_rendered_worker_ready = false;
    g_rendered_rx_claimed = false;

    lv_obj_t *scr = parent;
    lv_obj_remove_style_all(scr);
    lv_obj_add_style(scr, &style_screen, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, 320, 32);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, 8, 0);
    lv_obj_set_style_pad_gap(header, 8, 0);
    lv_obj_set_style_bg_color(header, COLOR_BG_CARD, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(header, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(header, 0, 0);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 40, 28);
    lv_obj_set_style_bg_color(back, COLOR_BG_CARD, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_border_color(back, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_lbl, COLOR_TEXT_BRIGHT, 0);
    lv_obj_center(back_lbl);
    bind_click(back_lbl, back_btn_cb, NULL);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "SD任务");
    lv_obj_set_style_text_font(title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(title, COLOR_TEXT_BRIGHT, 0);

    lv_obj_t *spacer = lv_obj_create(header);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_flex_grow(spacer, 1);

    create_header_btn(header, "在线", online_btn_cb);
    create_header_btn(header, "刷新", refresh_btn_cb);

    lv_obj_t *body = lv_obj_create(scr);
    panel_page_body_setup(body, 6);

    lv_obj_t *status_card = lv_obj_create(body);
    lv_obj_set_size(status_card, 290, 90);
    lv_obj_remove_flag(status_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(status_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(status_card, 4, 0);
    lv_obj_add_style(status_card, &style_card, 0);

    g_lbl_status = lv_label_create(status_card);
    lv_label_set_text(g_lbl_status, "SD任务未扫描");
    lv_obj_set_style_text_font(g_lbl_status, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(g_lbl_status, COLOR_LASER_ORANGE, 0);

    g_lbl_selected = lv_label_create(status_card);
    lv_label_set_text(g_lbl_selected, "未选择文件");
    lv_obj_set_style_text_font(g_lbl_selected, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(g_lbl_selected, COLOR_TEXT_LIGHT, 0);
    lv_obj_set_width(g_lbl_selected, 270);
    lv_label_set_long_mode(g_lbl_selected, LV_LABEL_LONG_DOT);

    lv_obj_t *action_row = lv_obj_create(status_card);
    lv_obj_set_size(action_row, 270, 32);
    lv_obj_remove_flag(action_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(action_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(action_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(action_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(action_row, 0, 0);
    lv_obj_set_style_pad_all(action_row, 0, 0);

    g_btn_start = create_action_btn(action_row, "启动", COLOR_LASER_GREEN,
                                    start_btn_cb, &g_lbl_start);
    g_btn_monitor = create_action_btn(action_row, "监控", COLOR_LASER_BLUE,
                                      monitor_btn_cb, &g_lbl_monitor);

    for (uint8_t i = 0; i < PANEL_FILE_MAX_COUNT; i++) {
        create_file_row(body, i);
    }

    lv_obj_t *preview_card = lv_obj_create(body);
    lv_obj_set_size(preview_card, 290, 70);
    lv_obj_remove_flag(preview_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(preview_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(preview_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(preview_card, 2, 0);
    lv_obj_add_style(preview_card, &style_card, 0);

    lv_obj_t *preview_title = lv_label_create(preview_card);
    lv_label_set_text(preview_title, "文件预览");
    lv_obj_set_style_text_font(preview_title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(preview_title, COLOR_LASER_BLUE, 0);

    g_lbl_preview = lv_label_create(preview_card);
    lv_label_set_text(g_lbl_preview, "--");
    lv_obj_set_style_text_font(g_lbl_preview, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(g_lbl_preview, COLOR_TEXT_MUTED, 0);
    lv_obj_set_width(g_lbl_preview, 270);
    lv_label_set_long_mode(g_lbl_preview, LV_LABEL_LONG_WRAP);
}

void page_file_browser_update(void)
{
    const panel_file_manager_t *mgr = panel_file_manager_get();
    char buf[96];
    bool busy = panel_offline_job_is_busy();
    bool tx_present = panel_transport_sle_tx_is_connected();
    bool rx_ready = panel_transport_sle_rx_is_connected();
    bool worker_ready = panel_offline_job_is_ready();
    bool rx_claimed = panel_transport_sle_can_control_rx();

    if (g_rendered_seq == mgr->seq &&
        g_rendered_selected_index == mgr->selected_index &&
        g_rendered_busy == busy &&
        g_rendered_tx_present == tx_present &&
        g_rendered_rx_ready == rx_ready &&
        g_rendered_worker_ready == worker_ready &&
        g_rendered_rx_claimed == rx_claimed) {
        return;
    }

    if (mgr->scanning) {
        snprintf(buf, sizeof(buf), "%s 正在扫描...", mgr->mount_label);
    } else {
        snprintf(buf, sizeof(buf), "%s %s | %u个文件 | %s",
                 mgr->mount_label,
                 mgr->mounted ? "已挂载" : "未挂载",
                 (unsigned int)mgr->count,
                 mgr->last_error);
    }
    lv_label_set_text(g_lbl_status, buf);
    lv_obj_set_style_text_color(g_lbl_status,
        mgr->scanning ? COLOR_LASER_BLUE :
        (mgr->mounted ? COLOR_LASER_ORANGE : COLOR_LASER_RED), 0);

    const panel_file_entry_t *selected = panel_file_manager_get_selected();
    bool has_selection = selected != NULL && !busy && !tx_present;
    bool can_start = has_selection && rx_ready && rx_claimed && worker_ready;
    if (selected != NULL) {
        if (busy) {
            snprintf(buf, sizeof(buf), "正在发送：%s", selected->name);
        } else if (!rx_ready) {
            snprintf(buf, sizeof(buf), "已选择：%s | 等待RX连接", selected->name);
        } else if (!rx_claimed) {
            snprintf(buf, sizeof(buf), "已选择：%s | 等待RX控制权", selected->name);
        } else if (!worker_ready) {
            snprintf(buf, sizeof(buf), "已选择：%s | 任务未就绪", selected->name);
        } else {
            snprintf(buf, sizeof(buf), "已选择：%s", selected->name);
        }
        lv_label_set_text(g_lbl_selected, buf);
    } else {
        lv_label_set_text(g_lbl_selected, "未选择文件");
    }

    lv_obj_set_style_bg_opa(g_btn_start, can_start ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_text_opa(g_lbl_start, can_start ? LV_OPA_COVER : LV_OPA_50, 0);
    if (can_start) {
        lv_obj_add_flag(g_btn_start, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(g_lbl_start, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(g_btn_start, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(g_lbl_start, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_style_bg_opa(g_btn_monitor, has_selection ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_text_opa(g_lbl_monitor, has_selection ? LV_OPA_COVER : LV_OPA_50, 0);

    for (uint8_t i = 0; i < PANEL_FILE_MAX_COUNT; i++) {
        const panel_file_entry_t *entry = panel_file_manager_get_entry(i);
        if (entry == NULL) {
            lv_obj_add_flag(g_rows[i].btn, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        lv_obj_remove_flag(g_rows[i].btn, LV_OBJ_FLAG_HIDDEN);
        snprintf(buf, sizeof(buf), "%s%s",
                 entry->selectable ? "" : "[只读] ",
                 entry->name);
        lv_label_set_text(g_rows[i].name, buf);
        lv_obj_set_style_text_color(g_rows[i].name,
            (mgr->selected_index == (int8_t)i) ? COLOR_LASER_GREEN :
            (entry->selectable ? COLOR_TEXT_BRIGHT : COLOR_TEXT_MUTED), 0);

        if (entry->line_count == 0U) {
            snprintf(buf, sizeof(buf), "%s | %lu B | 待统计",
                     panel_file_manager_type_text(entry->type),
                     (unsigned long)entry->size_bytes);
        } else {
            snprintf(buf, sizeof(buf), "%s | %lu B | %lu lines",
                     panel_file_manager_type_text(entry->type),
                     (unsigned long)entry->size_bytes,
                     (unsigned long)entry->line_count);
        }
        lv_label_set_text(g_rows[i].meta, buf);
    }

    if (busy) {
        lv_label_set_text(g_lbl_preview, "离线任务执行中，预览暂停");
    } else if (mgr->scanning) {
        lv_label_set_text(g_lbl_preview, "正在扫描SD卡...");
    } else if (selected != NULL && mgr->selected_index >= 0) {
        char preview[PANEL_FILE_PREVIEW_MAX];
        if (panel_file_manager_read_preview((uint8_t)mgr->selected_index,
                                            preview, sizeof(preview))) {
            lv_label_set_text(g_lbl_preview, preview);
        } else {
            lv_label_set_text(g_lbl_preview, "预览失败");
        }
    } else {
        lv_label_set_text(g_lbl_preview, "选择SD任务后显示预览");
    }

    g_rendered_seq = mgr->seq;
    g_rendered_selected_index = mgr->selected_index;
    g_rendered_busy = busy;
    g_rendered_tx_present = tx_present;
    g_rendered_rx_ready = rx_ready;
    g_rendered_worker_ready = worker_ready;
    g_rendered_rx_claimed = rx_claimed;
}
