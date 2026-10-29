#!/usr/bin/env bash
# Install the development tools; application builds remain a separate step.
set -euo pipefail
SETUP_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$SETUP_DIR/common.sh"
setup_main raspberry-pi "$@"
