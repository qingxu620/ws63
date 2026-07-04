/**
 * @file config.h
 * @brief Minimal config for refactor - screen_config.h re-export.
 */
#ifndef SCREEN_PANEL_CONFIG_H
#define SCREEN_PANEL_CONFIG_H

#include "screen_config.h"

/*
 * Keep SLE enabled for the product panel. The transport layer gates the
 * Screen->RX client path; normal Host jobs use only TX->Screen status mirror.
 */
#define PANEL_ENABLE_SLE 1

#endif
