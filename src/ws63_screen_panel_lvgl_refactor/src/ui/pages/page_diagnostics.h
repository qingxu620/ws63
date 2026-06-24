/**
 * @file page_diagnostics.h
 * @brief Diagnostics page: SLE stats, memory, connection info.
 */
#ifndef PAGE_DIAGNOSTICS_H
#define PAGE_DIAGNOSTICS_H

#include "lvgl.h"

void page_diagnostics_create(lv_obj_t *parent);
void page_diagnostics_update(void);

#endif
