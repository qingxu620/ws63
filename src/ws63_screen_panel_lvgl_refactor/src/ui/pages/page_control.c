/**
 * @file page_control.c
 * @brief Control Panel page: frame scan + manual jog buttons.
 */
#include "page_control.h"
#include "panel_theme.h"
#include "ui_manager.h"
#include "../service/panel_model.h"
#include "../service/panel_offline_job.h"
#include "../service/panel_transport_sle.h"
#include "soc_osal.h"

static lv_obj_t *g_lbl_frame_state;
static lv_obj_t *g_btn_frame_scan;
static lv_obj_t *g_lbl_frame_scan;
static uint32_t g_rendered_seq = UINT32_MAX;

static void bind_click(lv_obj_t *obj, lv_event_cb_t cb, void *user_data)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(obj, 6);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, user_data);
}

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    osal_printk("[CONTROL] back -> home\r\n");
    ui_manager_switch_page(PAGE_HOME);
}

static void frame_scan_cb(lv_event_t *e)
{
    (void)e;
    if (panel_offline_job_is_busy()) {
        lv_label_set_text(g_lbl_frame_state, "任务忙");
        lv_obj_set_style_text_color(g_lbl_frame_state, COLOR_LASER_ORANGE, 0);
        return;
    }
    if (!panel_transport_sle_can_control_rx()) {
        lv_label_set_text(g_lbl_frame_state, "仅离线可用");
        lv_obj_set_style_text_color(g_lbl_frame_state, COLOR_LASER_ORANGE, 0);
        return;
    }
    if (panel_offline_job_start_frame_scan() == ERRCODE_SUCC) {
        lv_label_set_text(g_lbl_frame_state, "边框扫描中");
        lv_obj_set_style_text_color(g_lbl_frame_state, COLOR_LASER_BLUE, 0);
        osal_printk("[CONTROL] frame scan queued\r\n");
    } else {
        lv_label_set_text(g_lbl_frame_state, "发送失败");
        lv_obj_set_style_text_color(g_lbl_frame_state, COLOR_LASER_RED, 0);
    }
}

static void jog_cb(lv_event_t *e)
{
    const char *dir = (const char *)lv_event_get_user_data(e);
    (void)dir;
}

static lv_obj_t *create_jog_btn(lv_obj_t *parent, const char *symbol, const char *dir)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 48, 36);
    lv_obj_set_style_bg_color(btn, COLOR_BG_CARD, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_color(btn, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_color(lbl, COLOR_TEXT_BRIGHT, 0);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, jog_cb, LV_EVENT_CLICKED, (void *)dir);
    bind_click(lbl, jog_cb, (void *)dir);
    return btn;
}

void page_control_create(lv_obj_t *parent)
{
    g_rendered_seq = UINT32_MAX;

    lv_obj_t *scr = parent;
    lv_obj_remove_style_all(scr);
    lv_obj_add_style(scr, &style_screen, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Header */
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
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_lbl, COLOR_TEXT_BRIGHT, 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    bind_click(back_lbl, back_btn_cb, NULL);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "手动控制");
    lv_obj_set_style_text_font(title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(title, COLOR_TEXT_BRIGHT, 0);

    /* Body */
    lv_obj_t *body = lv_obj_create(scr);
    panel_page_body_setup(body, 8);

    /* Frame scan card */
    lv_obj_t *frame_card = lv_obj_create(body);
    lv_obj_set_size(frame_card, 290, 80);
    lv_obj_remove_flag(frame_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(frame_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(frame_card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(frame_card, 14, 0);
    lv_obj_add_style(frame_card, &style_card, 0);

    lv_obj_t *info = lv_obj_create(frame_card);
    lv_obj_set_size(info, 160, 56);
    lv_obj_remove_flag(info, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(info, 5, 0);
    lv_obj_set_style_pad_all(info, 0, 0);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info, 0, 0);

    lv_obj_t *title_frame = lv_label_create(info);
    lv_label_set_text(title_frame, LV_SYMBOL_LOOP " 扫描边框");
    lv_obj_set_style_text_font(title_frame, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(title_frame, COLOR_LASER_BLUE, 0);

    lv_obj_t *detail = lv_label_create(info);
    lv_label_set_text(detail, "99mm矩形预览");
    lv_obj_set_style_text_font(detail, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(detail, COLOR_TEXT_MUTED, 0);

    g_lbl_frame_state = lv_label_create(info);
    lv_label_set_text(g_lbl_frame_state, "离线空闲可用");
    lv_obj_set_style_text_font(g_lbl_frame_state, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(g_lbl_frame_state, COLOR_LASER_GREEN, 0);

    g_btn_frame_scan = lv_button_create(frame_card);
    lv_obj_remove_style_all(g_btn_frame_scan);
    lv_obj_set_size(g_btn_frame_scan, 96, 44);
    lv_obj_set_style_bg_color(g_btn_frame_scan, COLOR_LASER_BLUE, 0);
    lv_obj_set_style_bg_opa(g_btn_frame_scan, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_btn_frame_scan, 8, 0);
    lv_obj_set_style_border_width(g_btn_frame_scan, 0, 0);
    lv_obj_add_event_cb(g_btn_frame_scan, frame_scan_cb, LV_EVENT_CLICKED, NULL);

    g_lbl_frame_scan = lv_label_create(g_btn_frame_scan);
    lv_label_set_text(g_lbl_frame_scan, "扫描");
    lv_obj_set_style_text_font(g_lbl_frame_scan, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(g_lbl_frame_scan, lv_color_white(), 0);
    lv_obj_center(g_lbl_frame_scan);
    bind_click(g_lbl_frame_scan, frame_scan_cb, NULL);

    /* Jog control card */
    lv_obj_t *jog_card = lv_obj_create(body);
    lv_obj_set_size(jog_card, 290, 104);
    lv_obj_remove_flag(jog_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(jog_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(jog_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(jog_card, 4, 0);
    lv_obj_add_style(jog_card, &style_card, 0);

    lv_obj_t *jog_title = lv_label_create(jog_card);
    lv_label_set_text(jog_title, LV_SYMBOL_LOOP " 手动点动（待接入）");
    lv_obj_set_style_text_font(jog_title, PANEL_FONT_CN, 0);
    lv_obj_set_style_text_color(jog_title, COLOR_LASER_BLUE, 0);

    /* Top row: spacer + UP + spacer */
    lv_obj_t *row_top = lv_obj_create(jog_card);
    lv_obj_set_size(row_top, 180, 32);
    lv_obj_remove_flag(row_top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row_top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_top, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row_top, 4, 0);
    lv_obj_set_style_bg_opa(row_top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row_top, 0, 0);
    lv_obj_set_style_pad_all(row_top, 0, 0);

    lv_obj_t *sp1 = lv_obj_create(row_top);
    lv_obj_set_size(sp1, 48, 1);
    lv_obj_set_style_bg_opa(sp1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp1, 0, 0);
    create_jog_btn(row_top, LV_SYMBOL_UP, "Y+");
    lv_obj_t *sp2 = lv_obj_create(row_top);
    lv_obj_set_size(sp2, 48, 1);
    lv_obj_set_style_bg_opa(sp2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp2, 0, 0);

    /* Middle row: LEFT + HOME + RIGHT */
    lv_obj_t *row_mid = lv_obj_create(jog_card);
    lv_obj_set_size(row_mid, 180, 32);
    lv_obj_remove_flag(row_mid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row_mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row_mid, 4, 0);
    lv_obj_set_style_bg_opa(row_mid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row_mid, 0, 0);
    lv_obj_set_style_pad_all(row_mid, 0, 0);

    create_jog_btn(row_mid, LV_SYMBOL_LEFT, "X-");
    create_jog_btn(row_mid, LV_SYMBOL_HOME, "HOME");
    create_jog_btn(row_mid, LV_SYMBOL_RIGHT, "X+");
}

void page_control_update(void)
{
    if (g_rendered_seq == g_model.seq) {
        return;
    }
    bool can_scan = panel_transport_sle_can_control_rx() && !panel_offline_job_is_busy();
    if (panel_offline_job_is_busy()) {
        lv_label_set_text(g_lbl_frame_state, "任务进行中");
        lv_obj_set_style_text_color(g_lbl_frame_state, COLOR_LASER_ORANGE, 0);
    } else if (!panel_transport_sle_can_control_rx()) {
        lv_label_set_text(g_lbl_frame_state, "仅离线可用");
        lv_obj_set_style_text_color(g_lbl_frame_state, COLOR_TEXT_MUTED, 0);
    } else {
        lv_label_set_text(g_lbl_frame_state, "离线空闲可用");
        lv_obj_set_style_text_color(g_lbl_frame_state, COLOR_LASER_GREEN, 0);
    }
    lv_obj_set_style_bg_opa(g_btn_frame_scan, can_scan ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_text_opa(g_lbl_frame_scan, can_scan ? LV_OPA_COVER : LV_OPA_50, 0);
    g_rendered_seq = g_model.seq;
}
