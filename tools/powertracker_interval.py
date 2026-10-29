#!/usr/bin/env python3
"""Calculate interval radio duty cycle from two cumulative PowerTracker snapshots."""

import argparse
import json
import re


def read_snapshot(path):
    """Read per-node counters, ignoring AVG and already-rounded percentages."""
    nodes = {}
    with open(path, encoding="utf-8-sig") as snapshot:
        for line in snapshot:
            match = re.match(r"^\s*(\S+)\s+(MONITORED|ON)\s+(\d+)\s+us\b", line)
            if not match or match[1] == "AVG":
                continue
            name, counter, value = match.groups()
            row = nodes.setdefault(name, {})
            if counter in row:
                raise ValueError(
                    f"Multiple snapshots in {path} for {name}: "
                    "save only one output per file"
                )
            row[counter] = int(value)
    if not nodes:
        raise ValueError(f"No per-node MONITORED/ON counters in {path}")
    if any(set(row) != {"MONITORED", "ON"} for row in nodes.values()):
        raise ValueError(f"Missing MONITORED or ON counter in {path}")
    return nodes


def compare_snapshots(start, end):
    """Use elapsed microseconds as weights when combining node duty cycles."""
    if start.keys() != end.keys():
        raise ValueError("Node sets differ; snapshots must be from the same run")
    rows = []
    total_on = 0
    total_monitored = 0
    for name in sorted(start):
        monitored = end[name]["MONITORED"] - start[name]["MONITORED"]
        radio_on = end[name]["ON"] - start[name]["ON"]
        if monitored <= 0 or radio_on < 0 or radio_on > monitored:
            raise ValueError(
                f"Invalid counter difference for {name}; "
                "check run, file order, reset or overflow"
            )
        rows.append(
            {
                "node": name,
                "measured_interval_seconds": monitored / 1e6,
                "radio_on_percent": 100 * radio_on / monitored,
            }
        )
        total_on += radio_on
        total_monitored += monitored
    return {
        "weighted_radio_on_percent": 100 * total_on / total_monitored,
        "nodes": rows,
        "note": (
            "Difference of cumulative counters. Radio duty cycle, not joules. "
            "Start/end must belong to the same uninterrupted run."
        ),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("start_file")
    parser.add_argument("end_file")
    args = parser.parse_args()
    try:
        result = compare_snapshots(
            read_snapshot(args.start_file), read_snapshot(args.end_file)
        )
    except (ValueError, OSError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
