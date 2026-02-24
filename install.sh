#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"

"${ROOT_DIR}/scripts/build_app.sh"
"${ROOT_DIR}/scripts/install_app.sh"

