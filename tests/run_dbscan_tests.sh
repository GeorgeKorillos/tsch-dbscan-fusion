#!/usr/bin/env bash
# Run the clustering group through the common test runner.
set -euo pipefail
if (( $# != 0 )); then
  echo 'Usage: bash tests/run_dbscan_tests.sh' >&2
  exit 2
fi
tests_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
exec bash "$tests_dir/run_tests.sh" dbscan
