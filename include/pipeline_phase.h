#ifndef PIPELINE_PHASE_H_
#define PIPELINE_PHASE_H_

#include "contiki.h"
#include "network_messages.h"

/* Stamp application phase and TSCH enqueue time into a BEACON. */
void pipeline_phase_stamp(control_message_t *msg, clock_time_t cycle_start);

/* Recover a recent parent cycle reference; return zero for stale/invalid stamps. */
int pipeline_phase_decode(const control_message_t *msg, clock_time_t *cycle_start);

#endif