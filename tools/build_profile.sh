#!/usr/bin/env bash
# Compile all three roles with one explicit set of scheduling and timing flags.
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_root"

usage() {
  cat <<'HELP'
Usage: bash tools/build_profile.sh PROFILE [LENGTH] [measure|diagnose]

Current profiles:
  shared-adaptive               Shared schedule, adaptive infrastructure EBs (default).
                                LENGTH: 3 (default) or 5.
  receiver-fixed                Receiver-based schedule, fixed infrastructure EBs.
                                LENGTH: 11, 17 (default) or 31.

Earlier comparisons, kept for reproducibility:
  shared-fixed-reference        Original timing; fixed EBs on all roles.
  shared-adaptive-reference     Original timing; adaptive EBs on all roles.
  receiver-fixed-reference      Original timing; fixed EBs on all roles.
  receiver-adaptive-reference   Original timing; adaptive EBs on all roles.
  shared-aligned                Extended timing, leaf EBs off, no sample-delay guard.
  receiver-fixed-aligned        As above, receiver schedule and fixed EBs.
  receiver-adaptive-aligned     As above, receiver schedule and adaptive EBs.

Earlier shared profiles always use three slots. Omit LENGTH or pass 3.
measure disables extra RX/coalescing diagnostics; diagnose enables them.
Every build cleans existing firmware. Run a simulation before building another profile.
HELP
}

if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
  usage
  exit 0
fi
if (( $# > 3 )); then
  usage >&2
  exit 2
fi

profile="${1:-shared-adaptive}"
mode="${3:-measure}"
shared_length=3
receiver_length=17
extended_timing=0
leaf_no_eb=0
sample_guard=0
compact_log=0

case "$mode" in
  measure) diagnostics=0 ;;
  diagnose) diagnostics=1 ;;
  *) echo 'Mode must be measure or diagnose.' >&2; exit 2 ;;
esac

# Reference profiles deliberately retain their original behavioral switches.
case "$profile" in
  shared-adaptive)
    dynamic=0; adaptive=1
    extended_timing=1; leaf_no_eb=1; sample_guard=1; compact_log=1
    shared_length="${2:-3}"
    ;;
  receiver-fixed)
    dynamic=1; adaptive=0
    extended_timing=1; leaf_no_eb=1; sample_guard=1; compact_log=1
    ;;
  shared-fixed-reference) dynamic=0; adaptive=0 ;;
  shared-adaptive-reference) dynamic=0; adaptive=1 ;;
  receiver-fixed-reference) dynamic=1; adaptive=0 ;;
  receiver-adaptive-reference) dynamic=1; adaptive=1 ;;
  shared-aligned)
    dynamic=0; adaptive=1; extended_timing=1; leaf_no_eb=1
    ;;
  receiver-fixed-aligned)
    dynamic=1; adaptive=0; extended_timing=1; leaf_no_eb=1
    ;;
  receiver-adaptive-aligned)
    dynamic=1; adaptive=1; extended_timing=1; leaf_no_eb=1
    ;;
  *) echo "Unknown profile: $profile" >&2; usage >&2; exit 2 ;;
esac

if (( dynamic )); then
  receiver_length="${2:-17}"
  case "$receiver_length" in
    11|17|31) ;;
    *) echo 'Receiver length must be 11, 17 or 31.' >&2; exit 2 ;;
  esac
  effective_length="$receiver_length"
else
  if [[ "$profile" != shared-adaptive && "${2:-3}" != 3 ]]; then
    echo 'This reference shared profile uses length 3; omit LENGTH or pass 3.' >&2
    exit 2
  fi
  case "$shared_length" in
    3|5) ;;
    *) echo 'Shared length must be 3 or 5.' >&2; exit 2 ;;
  esac
  effective_length="$shared_length"
fi

# Keep optional diagnostics within the tight Z1 ROM budget.
if (( diagnostics )); then
  compact_log=1
fi

flags=(
  USE_DBSCAN=1 USE_CSV_TRACES=1 SENSOR_ID_OFFSET=5
  PIPE_SAMPLE_PHASE_FIX="$sample_guard"
  PIPE_COMPACT_LOG="$compact_log"
  PIPE_SHARED_LENGTH="$shared_length"
  PIPE_EXTENDED_TIMING="$extended_timing"
  PIPE_DIAGNOSTICS="$diagnostics"
  PIPE_LEAF_NO_EB="$leaf_no_eb"
  PIPE_DYNAMIC_SCHEDULE="$dynamic"
  PIPE_ADAPTIVE_EB="$adaptive"
  PIPE_SCHEDULE_LENGTH="$receiver_length"
)

printf 'BUILD profile=%s length=%s mode=%s dynamic=%s adaptive=%s\n' \
  "$profile" "$effective_length" "$mode" "$dynamic" "$adaptive"
printf 'extended_timing=%s leaf_no_eb=%s sample_guard=%s compact=%s diagnostics=%s\n' \
  "$extended_timing" "$leaf_no_eb" "$sample_guard" "$compact_log" "$diagnostics"

for role in master slave sensor; do
  make -C "src/$role" TARGET=z1 clean
  make -C "src/$role" TARGET=z1 all "${flags[@]}"
done
for role in master slave sensor; do
  "${SIZE:-size}" "src/$role/build/z1/${role}_node.z1"
done

printf '\nIf Cooja recompiles, use the same flags after TARGET=z1:\n'
printf '%s ' "${flags[@]}"
printf '\n'
