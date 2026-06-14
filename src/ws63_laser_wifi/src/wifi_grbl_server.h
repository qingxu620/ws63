/**
 * @file wifi_grbl_server.h
 * @brief WiFi TCP Grbl server for the WS63 laser marker sample.
 */
#ifndef WIFI_GRBL_SERVER_H
#define WIFI_GRBL_SERVER_H

#include "errcode.h"

#ifdef __cplusplus
extern "C" {
#endif

errcode_t wifi_grbl_server_init(void);
int task_wifi_grbl_entry(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_GRBL_SERVER_H */
