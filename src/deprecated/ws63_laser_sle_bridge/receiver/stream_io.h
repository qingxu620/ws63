/**
 * @file stream_io.h
 * @brief Raw byte stream entry for the SLE bridge receiver.
 */
#ifndef SLE_BRIDGE_STREAM_IO_H
#define SLE_BRIDGE_STREAM_IO_H

#include "errcode.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef errcode_t (*stream_io_write_cb_t)(const void *data, uint16_t len);

errcode_t stream_io_init(stream_io_write_cb_t write_cb);
int stream_io_task(void *arg);
void stream_io_receive(const uint8_t *data, uint16_t len);
uint16_t stream_io_available(void);
void stream_io_notify_connected(void);
void stream_io_notify_disconnected(void);

#ifdef __cplusplus
}
#endif

#endif /* SLE_BRIDGE_STREAM_IO_H */
