#!/usr/bin/env bash
# ============================================================================
# restart.sh  —  重启 RaspberryPi5 视觉系统
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "正在重启 visionpost ..."
"${SCRIPT_DIR}/stop.sh"
sleep 1
"${SCRIPT_DIR}/start.sh" "$@"
