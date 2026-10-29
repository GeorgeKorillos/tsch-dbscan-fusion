#!/usr/bin/env python3
"""Convert the original semicolon-separated sensor CSV into a C lookup table.

The export has seven header rows, then a timestamp and ten sensor columns.
The input CSV is not included in this source archive; the generated header is.
"""

import argparse
import csv
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent
DEFAULT_INPUT = (
    PROJECT_ROOT / "trace/sensor_stat_9c9e1035-650d-470e-ac5a-5eb52948562d.csv"
)
DEFAULT_OUTPUT = PROJECT_ROOT / "include/sensor_traces.h"


def read_samples(path):
    samples = []
    with path.open(newline="", encoding="utf-8-sig") as source:
        rows = list(csv.reader(source, delimiter=";"))
    for line_number, row in enumerate(rows[7:], start=8):
        if len(row) <= 1 or row[1] == "":
            continue
        if len(row) < 11:
            raise ValueError(f"Row {line_number} has fewer than ten sensor values")
        # Preserve the original conversion: scale by 100 and truncate toward zero.
        values = [int(float(value.replace(",", ".")) * 100) for value in row[1:11]]
        if any(value < 0 or value > 32767 for value in values):
            raise ValueError(
                f"Row {line_number} is outside the nonnegative int16 DATA range"
            )
        samples.append(values)
    if not samples:
        raise ValueError("No sensor samples found after the seven header rows")
    return samples


def write_header(path, samples):
    # Transpose time-major CSV rows into sensor-major arrays for mote lookup.
    sensor_count = len(samples[0])
    with path.open("w", encoding="utf-8") as header:
        header.write("#ifndef SENSOR_TRACES_H\n#define SENSOR_TRACES_H\n\n")
        header.write("#include <stdint.h>\n\n")
        header.write(f"#define TRACE_SAMPLES {len(samples)}\n")
        header.write(f"#define TRACE_SENSORS {sensor_count}\n\n")
        header.write("/* Rows are sensors; each value is in hundredths. */\n")
        header.write(
            "static const uint16_t sensor_traces[TRACE_SENSORS][TRACE_SAMPLES] = {\n"
        )
        for sensor in range(sensor_count):
            header.write("  {\n")
            values = [str(row[sensor]) for row in samples]
            for offset in range(0, len(values), 12):
                header.write("    " + ", ".join(values[offset : offset + 12]) + ",\n")
            header.write("  },\n")
        header.write("};\n\n#endif\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", nargs="?", type=Path, default=DEFAULT_INPUT)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    try:
        samples = read_samples(args.input)
        write_header(args.output, samples)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f"Generated {args.output}: {len(samples)} samples, scale 100.")


if __name__ == "__main__":
    main()
