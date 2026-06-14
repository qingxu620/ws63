/**
 * @file sle_receiver.h
 * @brief SLE receiver module - receives G-code and sends response back.
 *
 * This module:
 *   - Receives G-code strings from transmitter via SLE
 *   - Sends response (ok/error/status) back to transmitter via SLE
 */
#ifndef SLE_RECEIVER_H
#define SLE_RECEIVER_H

#include "errcode.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SLE service/property UUIDs - must match transmitter */
#define SLE_RECEIVER_SERVICE_UUID    0x1A0B
#define SLE_RECEIVER_DATA_CHAR_UUID  0x1A01   /* RX: G-code data from transmitter */
#define SLE_RECEIVER_RESP_CHAR_UUID  0x1A02   /* TX: response to transmitter */

/* SLE device name for advertising */
#define SLE_RECEIVER_NAME "sle_laser_rx"

/**
 * @brief Initialize SLE server and start advertising
 * @return ERRCODE_SUCC on success
 */
errcode_t sle_receiver_init(void);

/**
 * @brief Check if transmitter is connected
 * @return true if connected
 */
bool sle_receiver_is_connected(void);

/**
 * @brief Send response data back to transmitter
 * @param data Response data (text or binary status packet)
 * @param len Length of data
 * @return ERRCODE_SUCC on success
 */
errcode_t sle_receiver_send_response(const void *data, uint16_t len);

/**
 * @brief Get connection status string for debug
 * @return Status string
 */
const char *sle_receiver_get_status(void);

#ifdef __cplusplus
}
#endif

#endif /* SLE_RECEIVER_H */
