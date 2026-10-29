#!/usr/bin/env bash
# Build real project modules against small host interfaces; never build firmware.
set -euo pipefail
project_root="$(cd -- "$(dirname -- "$0")/.." && pwd)"
usage() {
  echo 'Usage: bash tests/run_tests.sh [all|scheduler|timing|dbscan|tools]'
  echo 'Default: all. Requires a host C compiler and Python 3 for the full suite.'
}
if (( $# > 1 )); then usage >&2; exit 2; fi
group="${1:-all}"
case "$group" in
  -h|--help) usage; exit 0 ;;
  all|scheduler|timing|dbscan|tools) ;;
  *) usage >&2; exit 2 ;;
esac
compiler="${CC:-gcc}"
if [[ "$group" != tools ]]; then
  command -v "$compiler" >/dev/null || { echo "Missing host compiler: $compiler" >&2; exit 1; }
fi
if [[ "$group" == all || "$group" == tools ]]; then
  command -v python3 >/dev/null || { echo 'Missing python3' >&2; exit 1; }
fi
work="$(mktemp -d "$project_root/tests/.run.XXXXXX")"
trap 'rm -rf -- "$work"' EXIT

run_scheduler() {
  echo 'Running scheduler/EB tests (144 configurations)...'
  local extended leaf role length dynamic adaptive
  for extended in 0 1; do
    for leaf in 0 1; do
      for role in master slave sensor; do
        for length in 11 17 31; do
          for dynamic in 0 1; do
            for adaptive in 0 1; do
              # The context file makes a failing matrix entry identifiable.
              printf 'role=%s length=%s dynamic=%s adaptive=%s extended=%s leaf_no_eb=%s\n' \
                "$role" "$length" "$dynamic" "$adaptive" "$extended" "$leaf" > "$work/context"
              if ! "$compiler" -std=c99 -Wall -Wextra -Werror \
                -Wno-unused-function -Wno-unused-variable \
                -DPIPE_EXTENDED_TIMING="$extended" -DPIPE_LEAF_NO_EB="$leaf" \
                -DPIPE_HOST_TEST=1 -DPIPE_DYNAMIC_SCHEDULE="$dynamic" \
                -DPIPE_ADAPTIVE_EB="$adaptive" -DPIPE_SCHEDULE_LENGTH="$length" \
                -include "$project_root/src/$role/project-conf.h" \
                -I"$project_root/tests/scheduler_stubs" -I"$project_root/include" \
                "$project_root/src/common/pipeline_schedule.c" \
                "$project_root/tests/test_schedule.c" -o "$work/schedule"; then
                cat "$work/context" >&2
                return 1
              fi
              if ! "$work/schedule" > "$work/output" 2>&1; then
                cat "$work/context" "$work/output" >&2
                return 1
              fi
            done
          done
        done
      done
    done
  done
  echo 'PASS: scheduler/EB tests (144 configurations)'
}

run_timing() {
  local width
  for width in 16 32; do
    "$compiler" -std=c99 -Wall -Wextra -Werror -DTEST_CLOCK_WIDTH="$width" \
      -I"$project_root/tests/timing_stubs" -I"$project_root/include" \
      "$project_root/tests/test_sample_timing.c" -o "$work/timing"
    "$work/timing"
  done
  echo 'PASS: timing tests (16-bit and 32-bit clocks)'
}

run_dbscan() {
  local csv_mode
  for csv_mode in 1 0; do
    "$compiler" -std=c99 -Wall -Wextra -Werror -DUSE_DBSCAN=1 -DUSE_CSV_TRACES="$csv_mode" \
      -I"$project_root/tests/clustering_stubs" -I"$project_root/include" \
      "$project_root/tests/test_dbscan.c" "$project_root/src/common/clustering.c" \
      -o "$work/dbscan"
    "$work/dbscan"
  done
  echo 'PASS: DBSCAN tests (scalar and eight-dimensional builds)'
}

run_tools() {
  PYTHONDONTWRITEBYTECODE=1 python3 "$project_root/tests/test_tools.py"
  echo 'PASS: analysis tools (3 fixtures)'
}

if [[ "$group" == all ]]; then
  run_scheduler
  run_timing
  run_dbscan
  run_tools
  echo 'PASS: all test groups'
else
  "run_$group"
fi
