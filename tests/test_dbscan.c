#include "clustering.h"
#include "net/ipv6/uip.h"

#include <assert.h>
#include <string.h>
static cluster_point_t points[MAX_QUEUE_ITEMS];
static master_payload_t output[MAX_CANDIDATE_CLUSTERS];
static uint8_t count;
static void run(const uint16_t *values, uint8_t n) {
    memset(points, 0, sizeof(points));
    memset(output, 0, sizeof(output));
    for (uint8_t i = 0; i < n; i++) {
        points[i].magnitudes[0] = values[i];
        points[i].weight = 1;
        points[i].origin_id = i + 1;
    }
    run_clustering(points, n, output, &count, 42);
}
int main(void) {
    const uint16_t chain[] = {0, 400, 800};
    run(chain, 3);
    assert(count == 1 && output[0].aggregated_nodes_count == 3);
    assert(uip_ntohs(output[0].final_magnitudes_net[0]) == 400);
    const uint16_t permuted[] = {800, 0, 400};
    run(permuted, 3);
    assert(count == 1 && output[0].aggregated_nodes_count == 3);
    const uint16_t boundary[] = {0, 500};
    run(boundary, 2);
    assert(count == 1);
    assert(!is_spatially_similar(points[0].magnitudes, points[1].magnitudes, 250000, 1));
    const uint16_t isolated[] = {0, 1000, 2000, 3000};
    run(isolated, 4);
    assert(count == 3 && !points[3].visited);
    run(chain, 0);
    assert(count == 0);
    run(chain, 1);
    assert(count == 1 && output[0].aggregated_nodes_count == 1);
    run(chain, 2);
    memset(points, 0, sizeof(points));
    points[0].weight = 2;
    points[1].weight = 1;
    points[1].magnitudes[0] = 300;
    run_clustering(points, 2, output, &count, 42);
    assert(count == 1 && output[0].aggregated_nodes_count == 3);
    assert(uip_ntohs(output[0].final_magnitudes_net[0]) == 100);
    return 0;
}
