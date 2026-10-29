#ifndef PIPELINE_SAMPLE_TIMING_H_
#define PIPELINE_SAMPLE_TIMING_H_

#include "contiki.h"
#include "network_messages.h"

/* The requested phase is modulo one period. Immediately after a sample,
 * a tiny positive phase correction belongs to the NEXT cycle, not to a
 * second sample a few ticks later. Large realignments may take 2.5..7.5s. */
static inline clock_time_t pipeline_sample_delay(clock_time_t delay) {
    if (delay == 0) {
        return AGGREGATION_TICK;
    }
    if (delay < AGGREGATION_TICK / 2) {
        delay += AGGREGATION_TICK;
    }
    return delay;
}

#endif