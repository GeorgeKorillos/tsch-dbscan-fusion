# DBSCAN with MinPts=1

The local clustering stage groups measurement records by epsilon connectivity.
It is implemented in `src/common/clustering.c` and is enabled by all named build
profiles. The distance is defined over measurements, not mote coordinates.

## Input and neighborhood

`cluster_point_t` contains host-order magnitudes, represented-sensor weight and
source metadata. A CSV record uses one magnitude; signal mode uses eight DFT
magnitudes. An input can be a raw observation or a cluster representative from a
child relay. The caller supplies a fresh batch with both traversal flags cleared.

Two records are neighbors when their squared Euclidean distance is at most
`SPATIAL_DEDUP_THRESHOLD_SQ`. With the scalar hundredths representation, the
configured 250000 corresponds to epsilon 5.00. No square root is required. The
predicate subtracts each squared coordinate difference from a remaining budget,
so a large vector distance cannot wrap an accumulated sum into a small value.

MinPts=1 counts the input point itself. Thus every point is a core point,
singletons remain clusters, and no point is rejected as density noise. Input
weight does not change the density criterion; it affects the representative.

## Region expansion

For each undiscovered point, the algorithm creates an output cluster and expands
its neighborhood using a fixed queue of indices. Each discovered point is marked
before insertion and can enter the queue only once. Every queued point is in
turn compared with the remaining inputs. This finds the connected component of
the epsilon-neighborhood graph.

At epsilon 5, inputs 20, 24 and 28 form one cluster because 24 connects the other
two. Connectivity does not impose an epsilon bound on the component diameter.
The output cluster ID and seed source ID follow input discovery order; they are
local identifiers, not persistent event identities.

For n input records and d dimensions, the neighborhood work is O(n²d), with an
O(n) index queue. Here n is bounded by five and d is one or eight. The queue
occupies five bytes of automatic storage, in addition to other locals and stack
frames; it is not the entire stack requirement of the function.

## Weighted output

Each output carries a represented-sensor count and a weighted magnitude vector.
The averaging helper accumulates contributions using integer division, which
truncates intermediate values. Membership is determined from the input records,
not the evolving average. Integer representative values can vary slightly with
traversal order even when component membership is unchanged.

The one-byte count supports weights up to 255. If an additional contribution
would overflow it, the helper logs a warning and rejects that contribution.
Traversal still marks the point as processed; the oversized aggregate is not
split into another packet. Zero-weight inputs contribute nothing to the mean.

## Capacity and scope

The caller may provide at most `MAX_QUEUE_ITEMS` (five) records. An oversized
batch is rejected. The output array holds `MAX_CANDIDATE_CLUSTERS` (three)
components. Once full, further disconnected components remain unprocessed and a
warning is emitted. This limit can make which components survive depend on input
order. Mathematical DBSCAN itself does not impose that output cap.

P2P fusion uses the separate `is_spatially_similar()` predicate with a strict
threshold. It compares local representatives and transfers contributions using
the peer protocol. Therefore the hierarchical pipeline is not a centralized
DBSCAN over every raw measurement. Its payloads do not preserve full provenance,
and clustering cannot establish unique sensor/epoch delivery.

`USE_DBSCAN=0` selects a single weighted mean over all inputs. This optional
comparison mode performs no neighborhood grouping; it is not another DBSCAN
variant. All profiles in `tools/build_profile.sh` set `USE_DBSCAN=1`.

## Validation

Run `bash tests/run_dbscan_tests.sh` from the project root. The tests compile the
active `src/common/clustering.c` in scalar and eight-dimensional modes. They
cover chained neighborhoods, a permuted chain, epsilon equality, strict P2P
comparison, isolated components, the output cap, empty and singleton inputs, and
weighted averaging. These are host checks, not radio or MSP430 stack tests.

The README Tests section describes the host suite, prerequisites and coverage.
Target builds and Cooja measurements are separate validation steps.

## Algorithm reference

Martin Ester, Hans-Peter Kriegel, Jörg Sander and Xiaowei Xu. *A Density-Based
Algorithm for Discovering Clusters in Large Spatial Databases with Noise*.
Proceedings of KDD 1996, pp. 226–231. Neighborhood and density-connectivity are
defined on pp. 227–228; region expansion appears on p. 229. The implementation
specializes that expansion to MinPts=1; weighted representative generation is
an application-level fusion operation.
