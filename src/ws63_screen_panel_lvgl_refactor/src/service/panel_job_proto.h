/**
 * @file panel_job_proto.h
 * @brief Shared SLE Job packet sequence allocator for panel-originated packets.
 */
#ifndef PANEL_JOB_PROTO_H
#define PANEL_JOB_PROTO_H

#include <stdint.h>

uint16_t panel_job_proto_next_seq(void);

#endif /* PANEL_JOB_PROTO_H */
