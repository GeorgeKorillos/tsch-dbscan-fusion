#!/usr/bin/env python3
"""Small regression fixtures for the two read-only analysis tools."""

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


delivery = load_tool("analyze_delivery")
power = load_tool("powertracker_interval")


class AnalysisTests(unittest.TestCase):
    def test_complete_and_truncated_reports(self):
        # A partially copied report must not be treated as packet loss.
        log = """00:01.000 ID:5 Sensor started with ID: 100
05:00.000 ID:5 Tx DATA (Seq 255) UPWARDS
05:00.200 ID:3 PIPE RX src=100 seq=255 cluster=0
05:01.000 ID:1 MASTER FINAL REPORT: Generated 1 Unique Event(s)
05:01.010 ID:1 Aggregated from 2 total sensors
05:06.000 ID:1 MASTER FINAL REPORT: Generated 0 Unique Event(s)
05:11.000 ID:1 MASTER FINAL REPORT: Generated 1 Unique Event(s)
"""
        with tempfile.TemporaryDirectory(dir=ROOT / "tests") as directory:
            path = Path(directory) / "log.txt"
            path.write_text(log)
            result = delivery.analyze(path, 300, 3600, 3)
        self.assertEqual(result["complete_logged_reports"], 2)
        self.assertEqual(result["excluded_incomplete_log_records"], 1)
        self.assertEqual(result["weight_deficit"], 4)
        self.assertEqual(result["matched_hop_delays"], 1)
        self.assertAlmostEqual(result["hop_delay_seconds_p50"], 0.2)

    def test_counter_differences(self):
        start = {"A": {"MONITORED": 100, "ON": 50}}
        end = {"A": {"MONITORED": 300, "ON": 70}}
        result = power.compare_snapshots(start, end)
        self.assertEqual(result["weighted_radio_on_percent"], 10)
        with self.assertRaises(ValueError):
            power.compare_snapshots(end, start)

    def test_duplicate_snapshot_is_rejected(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "tests") as directory:
            path = Path(directory) / "power.txt"
            path.write_text("Z1_1 MONITORED 100 us\nZ1_1 ON 5 us\nZ1_1 ON 6 us\n")
            with self.assertRaises(ValueError):
                power.read_snapshot(path)


if __name__ == "__main__":
    unittest.main(argv=sys.argv)
