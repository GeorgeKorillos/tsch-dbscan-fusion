#!/usr/bin/env python3
"""Summarize aggregate weights and optional application-hop diagnostics.

A complete report is a header followed by the declared number of event lines.
Weights measure represented contributions, not unique sensor/epoch delivery.
"""

import argparse
import collections
import json
import re

MARKERS = (
    "add packet failed",
    "Queue Full",
    "Parent Timeout",
    "Migration!",
    "stale MERGE",
    "MERGE rejected",
    "PIPE COALESCE",
    "DYN common fallback",
    "skipped overdue",
)


def quantile(values, fraction):
    """Select an observed value; do not interpolate between log timestamps."""
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int((len(ordered) - 1) * fraction))]


def analyze(log_path, start_seconds, end_seconds, expected_weight):
    reports = []
    current_report = None
    application_ids = {}
    transmissions = {}
    hop_delays = []
    markers = collections.Counter()
    report_ticks = []
    rx_count = 0
    unmatched_rx_count = 0

    # Read from boot even when the measurement window starts later. Startup IDs
    # and earlier sends are needed to interpret receptions near the left edge.
    with open(log_path, encoding="utf-8-sig") as log_file:
        for line in log_file:
            timestamp = re.match(r"(\d+):(\d+\.\d+)\s+ID:(\d+)", line)
            if not timestamp:
                continue
            seconds = int(timestamp[1]) * 60 + float(timestamp[2])
            node = int(timestamp[3])
            in_window = start_seconds <= seconds < end_seconds

            match = re.search(r"(?:SLAVE|Sensor|MASTER) started with ID: (\d+)", line)
            if match:
                application_ids[node] = int(match[1])

            # The simulator ID differs from the application's MAC-derived ID.
            match = re.search(r"Tx CLUSTER ID (\d+) \(Nodes: (\d+), Seq: (\d+)\)", line)
            if match and node in application_ids:
                key = (application_ids[node], int(match[3]), int(match[1]))
                transmissions[key] = seconds
            match = re.search(r"Tx DATA \(Seq (\d+)\) UPWARDS", line)
            if match and node in application_ids:
                transmissions[(application_ids[node], int(match[1]), 0)] = seconds

            # Match only the latest occurrence of a key and a short delay. This
            # limits sequence-wrap ambiguity, but is not an end-to-end PDR test.
            match = re.search(r"PIPE RX src=(\d+) seq=(\d+) cluster=(\d+)", line)
            if match and in_window:
                rx_count += 1
                key = tuple(map(int, match.groups()))
                sent_at = transmissions.get(key)
                if sent_at is not None and 0 <= seconds - sent_at < 30:
                    hop_delays.append(seconds - sent_at)
                else:
                    unmatched_rx_count += 1

            match = re.search(r"MASTER FINAL REPORT: Generated (\d+) Unique", line)
            if match:
                current_report = {
                    "time": seconds,
                    "events": int(match[1]),
                    "weights": [],
                }
                reports.append(current_report)
            match = re.search(r"Aggregated from (\d+) total sensors", line)
            if match and current_report is not None:
                current_report["weights"].append(int(match[1]))

            match = re.search(r"PIPE report=\d+ scheduled=(\d+) actual=(\d+)", line)
            if match and in_window:
                report_ticks.append(tuple(map(int, match.groups())))
            if in_window:
                for marker in MARKERS:
                    if marker in line:
                        markers[marker] += 1

    selected = [r for r in reports if start_seconds <= r["time"] < end_seconds]
    complete = [r for r in selected if len(r["weights"]) == r["events"]]
    weights = [sum(r["weights"]) for r in complete]
    report_count = len(weights)
    deficit = sum(max(0, expected_weight - weight) for weight in weights)

    # Keep excess separate: an oversized report cannot repair a missing sample
    # in another report. Tick differences use unsigned 32-bit wrap arithmetic.
    scheduled_deltas = collections.Counter(
        (next_tick[0] - previous_tick[0]) & 0xFFFFFFFF
        for previous_tick, next_tick in zip(report_ticks, report_ticks[1:])
    )
    lateness = collections.Counter(
        (actual - scheduled) & 0xFFFFFFFF for scheduled, actual in report_ticks
    )
    return {
        "start_seconds": start_seconds,
        "end_seconds": end_seconds,
        "complete_logged_reports": report_count,
        "excluded_incomplete_log_records": len(selected) - report_count,
        "total_weight_histogram": dict(sorted(collections.Counter(weights).items())),
        "weight_deficit": deficit,
        "weight_excess": sum(max(0, weight - expected_weight) for weight in weights),
        "deficit_percent": (
            100 * deficit / (expected_weight * report_count) if report_count else None
        ),
        "scheduled_delta_ticks": dict(scheduled_deltas),
        "lateness_ticks": dict(lateness),
        "markers": dict(markers),
        "diagnostic_rx_records": rx_count,
        "matched_hop_delays": len(hop_delays),
        "unmatched_rx_records": unmatched_rx_count,
        "hop_delay_seconds_p50": quantile(hop_delays, 0.5),
        "hop_delay_seconds_p95": quantile(hop_delays, 0.95),
        "hop_delay_seconds_max": max(hop_delays, default=None),
        "note": (
            "Aggregate weights are not unique sensor/epoch PDR. RX means application "
            "enqueue. Hop delays match source/8-bit seq/cluster within 30s; "
            "no end-to-end epoch attribution."
        ),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log")
    parser.add_argument("--start-seconds", type=float, default=300)
    parser.add_argument("--end-seconds", type=float, default=3600)
    parser.add_argument("--expected-weight", type=int, default=3)
    args = parser.parse_args()
    if args.expected_weight <= 0 or args.end_seconds <= args.start_seconds:
        parser.error("Expected weight must be positive and end must be after start.")
    try:
        result = analyze(
            args.log, args.start_seconds, args.end_seconds, args.expected_weight
        )
    except OSError as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
