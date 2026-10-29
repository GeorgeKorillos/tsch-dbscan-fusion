#include "clustering.h"

#include "net/ipv6/uip.h"
#include "sys/log.h"

#include <string.h>

#ifndef USE_DBSCAN
#    define USE_DBSCAN 1
#endif

#ifndef USE_CSV_TRACES
#    define USE_CSV_TRACES 1
#endif

#define LOG_MODULE "Clustering"
#define LOG_LEVEL LOG_LEVEL_INFO

/* CSV input is scalar; synthetic signal input uses all frequency bins. */
static uint8_t get_dimensions() {
    return (USE_CSV_TRACES == 1) ? 1 : FFT_SAMPLES;
}

/* Reject distances at or above the threshold without computing a square root. */
bool is_spatially_similar(uint16_t *p1, uint16_t *p2, uint32_t threshold_sq, uint8_t dimensions) {
    uint32_t diff_sum = 0;
    for (int i = 0; i < dimensions; i++) {
        int32_t diff = (int32_t)p1[i] - (int32_t)p2[i];
        uint32_t magnitude = diff < 0 ? (uint32_t)(-diff) : (uint32_t)diff;
        uint32_t square = magnitude * magnitude;
        if (diff_sum >= threshold_sq || square >= threshold_sq - diff_sum) {
            return false;
        }
        diff_sum += square;
    }
    return (diff_sum < threshold_sq);
}

/* Use represented sensor counts as weights. Integer division truncates; reject
 * a contribution that would overflow the one-byte count on the wire. */
static void compute_weighted_average(master_payload_t *cluster, cluster_point_t *point,
                                     uint8_t dimensions) {
    uint32_t total_weight = cluster->aggregated_nodes_count + point->weight;
    if (total_weight == 0) {
        return;
    }
    if (total_weight > 255) {
        LOG_WARN("Fusion weight overflow; contribution dropped\n");
        return;
    }

    for (int i = 0; i < dimensions; i++) {
        uint32_t current_val = uip_ntohs(cluster->final_magnitudes_net[i]);
        uint32_t new_val = ((current_val * cluster->aggregated_nodes_count) +
                            ((uint32_t)point->magnitudes[i] * point->weight)) /
                           total_weight;
        cluster->final_magnitudes_net[i] = uip_htons((uint16_t)new_val);
    }
    cluster->aggregated_nodes_count = total_weight;
}

/* The DBSCAN paper includes the epsilon boundary. Keep this separate from
 * is_spatially_similar(): P2P continues to use its existing strict threshold. */
#if USE_DBSCAN == 1
static bool within_epsilon(const uint16_t *a, const uint16_t *b, uint8_t dimensions) {
    uint32_t remaining = SPATIAL_DEDUP_THRESHOLD_SQ;
    for (uint8_t d = 0; d < dimensions; d++) {
        int32_t delta = (int32_t)a[d] - (int32_t)b[d];
        uint32_t magnitude = delta < 0 ? (uint32_t)(-delta) : (uint32_t)delta;
        uint32_t square = magnitude * magnitude;
        if (square > remaining) {
            return false;
        }
        remaining -= square;
    }
    return true;
}
#endif

void run_clustering(cluster_point_t *points, uint8_t num_points, master_payload_t *out_clusters,
                    uint8_t *out_cluster_count, uint16_t my_slave_id) {
    *out_cluster_count = 0;
    uint8_t dimension_count = get_dimensions();

#if USE_DBSCAN == 1
    /* DBSCAN with MinPts=1: every input is a core point, including singletons.
     * Expand connected epsilon-neighborhoods using a bounded index queue. */
    if (num_points > MAX_QUEUE_ITEMS) {
        LOG_WARN("Too many clustering inputs\n");
        return;
    }
    uint8_t pending[MAX_QUEUE_ITEMS];
    for (uint8_t i = 0; i < num_points; i++) {
        if (points[i].visited) {
            continue;
        }

        /* The output array has a fixed capacity; leave remaining points unprocessed when it
         * fills. */
        if (*out_cluster_count >= MAX_CANDIDATE_CLUSTERS) {
            LOG_WARN("Cluster limit; remaining inputs ignored\n");
            break;
        }

        points[i].visited = true;

        /* Start a cluster from the next unvisited point. */
        master_payload_t *new_cluster = &out_clusters[*out_cluster_count];
        new_cluster->msg_type = MSG_TYPE_PROCESSED_DATA;
        new_cluster->slave_id = uip_htons(my_slave_id);
        new_cluster->sensor_id = uip_htons(points[i].origin_id);
        new_cluster->cluster_id = *out_cluster_count;
        new_cluster->aggregated_nodes_count =
            0; /* The first weighted contribution supplies the initial count. */
        for (int d = 0; d < dimension_count; d++) {
            new_cluster->final_magnitudes_net[d] = uip_htons(0);
        }

        /* Include the seed before looking for nearby points. */
        compute_weighted_average(new_cluster, &points[i], dimension_count);
        points[i].clustered = true;

        /* Mark on insertion: each point enters the queue once. Search around
         * every discovered point, not just the initial seed or the centroid. */
        uint8_t head = 0;
        uint8_t tail = 0;
        pending[tail++] = i;
        while (head < tail) {
            uint8_t current = pending[head++];
            for (uint8_t j = 0; j < num_points; j++) {
                if (!points[j].visited && within_epsilon(points[current].magnitudes,
                                                         points[j].magnitudes, dimension_count)) {
                    points[j].visited = true;
                    pending[tail++] = j;
                    compute_weighted_average(new_cluster, &points[j], dimension_count);
                    points[j].clustered = true;
                }
            }
        }
        (*out_cluster_count)++;
    }
#else
    /* Alternative mode: combine every input point into one weighted average. */
    if (num_points == 0) {
        return;
    }

    master_payload_t *new_cluster = &out_clusters[0];
    new_cluster->msg_type = MSG_TYPE_PROCESSED_DATA;
    new_cluster->slave_id = uip_htons(my_slave_id);
    new_cluster->sensor_id = uip_htons(points[0].origin_id);
    new_cluster->cluster_id = *out_cluster_count;
    new_cluster->aggregated_nodes_count = 0;
    for (int d = 0; d < dimension_count; d++) {
        new_cluster->final_magnitudes_net[d] = uip_htons(0);
    }

    for (uint8_t i = 0; i < num_points; i++) {
        compute_weighted_average(new_cluster, &points[i], dimension_count);
    }
    *out_cluster_count = 1;
#endif
}
