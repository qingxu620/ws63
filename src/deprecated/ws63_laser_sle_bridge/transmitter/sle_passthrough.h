/**
 * @file sle_passthrough.h
 * @brief Bidirectional SLE-UART transparent byte bridge.
 *
 * This module acts as a "wireless serial port":
 *   UART RX -> SLE TX
 *   SLE RX  -> UART TX
 */
#ifndef SLE_PASSTHROUGH_H
#define SLE_PASSTHROUGH_H

#include "errcode.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SLE service/property UUIDs - must match receiver */
#define SLE_PASSTHROUGH_SERVICE_UUID    0x1A0B
#define SLE_PASSTHROUGH_DATA_CHAR_UUID  0x1A01   /* TX -> RX raw bytes */
#define SLE_PASSTHROUGH_RESP_CHAR_UUID  0x1A02   /* RX -> TX raw bytes */

/* SLE device name for receiver to discover */
#define SLE_RECEIVER_NAME "sle_laser_rx"

/**
 * @brief Initialize SLE client and start scanning for receiver
 * @return ERRCODE_SUCC on success
 */
errcode_t sle_passthrough_init(void);

/**
 * @brief Send raw bytes to receiver via SLE
 * @param line Byte buffer
 * @param len Byte count
 * @return ERRCODE_SUCC on success
 */
errcode_t sle_passthrough_send_line(const char *line, uint16_t len);

/**
 * @brief Send raw bytes with SLE write command, without stack-level write confirmation.
 * @param data Byte buffer
 * @param len Byte count
 * @return ERRCODE_SUCC on submit success
 */
errcode_t sle_passthrough_send_cmd(const void *data, uint16_t len);

/**
 * @brief Check if SLE is connected to receiver
 * @return true if connected
 */
bool sle_passthrough_is_connected(void);

/**
 * @brief Retry receiver connection when SLE is enabled but link is not ready.
 *
 * Safe to call periodically from a monitor task.
 */
void sle_passthrough_poll_connect(void);

/**
 * @brief Get connection status string for debug
 * @return Status string
 */
const char *sle_passthrough_get_status(void);

/**
 * @brief Callback for receiving raw bytes from receiver
 * @param data Response bytes
 * @param length Length of data
 */
typedef void (*sle_response_cb_t)(const uint8_t *data, uint16_t length);

/**
 * @brief Set callback for raw bytes from receiver
 * @param cb Callback function
 */
void sle_passthrough_set_response_cb(sle_response_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* SLE_PASSTHROUGH_H */
