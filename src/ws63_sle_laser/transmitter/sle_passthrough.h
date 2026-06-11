/**
 * @file sle_passthrough.h
 * @brief Bidirectional SLE-UART bridge for G-code passthrough.
 *
 * This module acts as a "wireless serial port":
 *   UART RX → SLE TX (G-code to receiver)
 *   SLE RX → UART TX (ok/error/status from receiver)
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
#define SLE_PASSTHROUGH_DATA_CHAR_UUID  0x1A01   /* TX: G-code data */
#define SLE_PASSTHROUGH_RESP_CHAR_UUID  0x1A02   /* RX: ok/error/status */

/* SLE device name for receiver to discover */
#define SLE_RECEIVER_NAME "sle_laser_rx"

/**
 * @brief Initialize SLE client and start scanning for receiver
 * @return ERRCODE_SUCC on success
 */
errcode_t sle_passthrough_init(void);

/**
 * @brief Send a G-code line to receiver via SLE
 * @param line G-code string (null-terminated)
 * @param len Length of the string
 * @return ERRCODE_SUCC on success
 */
errcode_t sle_passthrough_send_line(const char *line, uint16_t len);

/**
 * @brief Check if SLE is connected to receiver
 * @return true if connected
 */
bool sle_passthrough_is_connected(void);

/**
 * @brief Get connection status string for debug
 * @return Status string
 */
const char *sle_passthrough_get_status(void);

/**
 * @brief Callback for receiving response data from receiver
 * @param data Response data (ok/error/status)
 * @param length Length of data
 */
typedef void (*sle_response_cb_t)(const uint8_t *data, uint16_t length);

/**
 * @brief Set callback for response data from receiver
 * @param cb Callback function
 */
void sle_passthrough_set_response_cb(sle_response_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* SLE_PASSTHROUGH_H */
