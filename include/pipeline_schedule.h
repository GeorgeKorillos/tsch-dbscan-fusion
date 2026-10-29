#ifndef PIPELINE_SCHEDULE_H_
#define PIPELINE_SCHEDULE_H_

#include <stdint.h>

struct tsch_neighbor;

/* Set up links before starting the MAC. A zero return means initialization failed. */
int pipeline_schedule_init(void);

/* Classify queued frames for the matching receiver cell or common control cells. */
int pipeline_schedule_packet_ready(void);

/* Request a short EB interval after a topology or association change. */
void pipeline_schedule_reset_eb(void);

void pipeline_schedule_new_time_source(const struct tsch_neighbor *, const struct tsch_neighbor *);

/* Periodic process-context maintenance for EB intervals and unused TX cells. */
void pipeline_schedule_maintenance(void);

#endif
