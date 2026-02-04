#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APP_NAME="OpenFlow"
APP_DIR="${ROOT_DIR}/dist/${APP_NAME}.app"
TARGET_DIR="${HOME}/Applications"

if [ ! -d "${APP_DIR}" ]; then
  echo "error: ${APP_DIR} not found. Run scripts/build_app.sh first."
  exit 1
fi

mkdir -p "${TARGET_DIR}"
rm -rf "${TARGET_DIR}/${APP_NAME}.app"
cp -R "${APP_DIR}" "${TARGET_DIR}/"

echo "✅ Installed to ${TARGET_DIR}/${APP_NAME}.app"
