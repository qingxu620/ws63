/**
 * @file diag_log.h
 * @brief Unified RX diagnostic log helpers.
 */
#ifndef WS63_LASER_RX_DIAG_LOG_H
#define WS63_LASER_RX_DIAG_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

void diag_log_info(const char *message);
void diag_log_error(const char *message);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_DIAG_LOG_H */
