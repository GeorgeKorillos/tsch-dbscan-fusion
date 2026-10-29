#ifndef NETWORK_MESSAGES_H_
#define NETWORK_MESSAGES_H_

#include "fft_processing.h"

#include <stdint.h>

/* UDP ports shared by the three application roles. */

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678
#define UDP_PEER_PORT 5680

#define MAX_CHILDREN 10
/* Expire inactive children so advertised load reflects recent traffic. */
#define INACTIVE_TIMEOUT 40
#define PARENT_TIMEOUT (180 * CLOCK_SECOND)

/* All application stages use the same five-second cycle. */
#define AGGREGATION_TICK (5 * CLOCK_SECOND)

/* Application beacons slow down after children have joined; keep a whole number of
 * cycles. */
/* 30 seconds */
#define BEACON_INTERVAL_SLOW (6 * AGGREGATION_TICK)
#define CHILDREN_THRESHOLD 1

#define OFFER_TIMEOUT (3 * CLOCK_SECOND)
#define LOAD_PENALTY 10
#define HYSTERESIS_MARGIN 15

/* Fixed-point values use hundredths of the original measurement unit. */

#define FIXED_POINT_SCALE 100

/* Compare squared distances: a difference of 5.00 becomes 500, whose square is 250000. */
#define SPATIAL_DEDUP_THRESHOLD_SQ 250000

/* Snapshot 1.5 seconds after the scheduled cycle start. */
#define AGGREGATION_WINDOW (3 * CLOCK_SECOND / 2)

/* Two seconds reserved for peer exchange. */
#define PEER_WAIT_WINDOW (2 * CLOCK_SECOND)

#if PIPE_EXTENDED_TIMING
/* Allow late uplinks until 4.75 seconds; keep the five-second period. */
#    define MASTER_AGGREGATION_WINDOW (19 * CLOCK_SECOND / 4)
#else
/* Reference profiles report four seconds after the cycle starts. */
#    define MASTER_AGGREGATION_WINDOW (4 * CLOCK_SECOND)
#endif

/* Bound queue and clustering storage to fit the Z1 RAM budget. */
/* Shared pool for pending input records. */
#define MAX_QUEUE_ITEMS 5

/* Stop creating clusters when this output array is full. */
#define MAX_CANDIDATE_CLUSTERS 3

/* Maximum distinct peer contributions recorded per cluster. */
#define MAX_PEERS_PER_CLUSTER 3

#define MSG_TYPE_BEACON 1
#define MSG_TYPE_JOIN 2
#define MSG_TYPE_OFFER 3
#define MSG_TYPE_DATA 4
#define MSG_TYPE_PROCESSED_DATA 5
#define MSG_TYPE_PEER_SNOOP 6
#define MSG_TYPE_PEER_MERGE 7

/* Discovery and parent selection. Multibyte wire fields use network byte order. */
typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    uint16_t node_id;
    uint8_t level;
    uint8_t load;
    /* BEACON only: ticks since the scheduled cycle start, in network byte order. */
    uint16_t phase_offset_net;
    uint32_t phase_sent_net; /* TSCH uptime sampled when the beacon is queued. */
} control_message_t;

/* One sensor sample. CSV mode uses the first element; signal mode uses all eight. */
typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    uint16_t node_id;
    uint8_t seq_no;
    int16_t raw_data_net[FFT_SAMPLES];
} data_payload_t;

/* A weighted cluster sent one hop toward the master, possibly through another slave. */
typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    uint16_t slave_id;
    uint16_t sensor_id;
    uint8_t cluster_id;
    uint8_t seq_no;
    uint8_t aggregated_nodes_count;
    uint16_t final_magnitudes_net[FFT_SAMPLES];
} master_payload_t;

/* Same-level peer exchange. SNOOP advertises a candidate; MERGE transfers a
 * contribution. */
typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    uint16_t slave_id;
    uint16_t target_id; /* Application ID of the intended MERGE receiver. */
    uint8_t level;
    uint8_t cluster_id;
    uint8_t aggregated_nodes_count;
    uint16_t magnitudes_net[FFT_SAMPLES];
    /* SNOOP advertises its window token; MERGE echoes that token. */
    uint16_t window_token_net;
    uint8_t source_cluster_id;
} peer_message_t;

#endif