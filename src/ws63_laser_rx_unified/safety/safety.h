/**
 * @file safety.h
 * @brief Unified RX safety helpers.
 */
#ifndef WS63_LASER_RX_SAFETY_H
#define WS63_LASER_RX_SAFETY_H

#ifdef __cplusplus
extern "C" {
#endif

void safety_init(void);
void safety_force_laser_off(void);
void safety_abort_all(void);
void safety_on_disconnect(void);

#ifdef __cplusplus
}
#endif

#endif /* WS63_LASER_RX_SAFETY_H */
