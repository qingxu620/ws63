/**
 * @file interpolator.h
 * @brief Receiver motion executor compatible with the original interpolator API.
 */
#ifndef INTERPOLATOR_H
#define INTERPOLATOR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void interpolator_init(void);
void perform_move(double target_x, double target_y, double feed_rate_mm_min);
double interpolator_get_x(void);
double interpolator_get_y(void);
bool interpolator_is_busy(void);
void interpolator_set_origin(void);
void interpolator_request_abort(void);
int task_interpolator_entry(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* INTERPOLATOR_H */
