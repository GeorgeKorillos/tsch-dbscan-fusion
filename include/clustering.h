#ifndef CLUSTERING_H_
#define CLUSTERING_H_

#include "fft_processing.h"
#include "network_messages.h"

#include <stdbool.h>
#include <stdint.h>

/* One DBSCAN input record in host byte order. A record can represent either
 * one raw sensor observation or a weighted aggregate received from a child. */
typedef struct {
    uint16_t magnitudes[FFT_SAMPLES];
    uint8_t weight;     /* Number of sensor contributions represented by this point. */
    uint16_t origin_id; /* Immediate source sensor or slave. */
    uint8_t cluster_id; /* Distinguishes several clusters from the same source. */
    uint8_t seq_no;     /* Used to keep the newer record when a source appears twice in a
                           snapshot. */
    bool visited;       /* Discovered and queued for epsilon-neighborhood expansion. */
    bool clustered;     /* Already included in an output cluster. */
} cluster_point_t;

/* Strict similarity predicate for P2P fusion; the DBSCAN neighborhood uses <=. */
bool is_spatially_similar(uint16_t *p1, uint16_t *p2, uint32_t threshold_sq, uint8_t dimensions);

/* Form local DBSCAN clusters with MinPts=1 and emit weighted representatives.
 * Inputs start with visited/clustered cleared. Storage is bounded by
 * MAX_QUEUE_ITEMS inputs and MAX_CANDIDATE_CLUSTERS outputs. */
void run_clustering(cluster_point_t *points, uint8_t num_points, master_payload_t *out_clusters,
                    uint8_t *out_cluster_count, uint16_t my_slave_id);

#endif