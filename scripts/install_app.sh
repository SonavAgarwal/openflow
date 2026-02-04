#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APP_NAME="OpenFlow"
APP_DIR="${ROOT_DIR}/dist/${APP_NAME}.app"
TARGET_DIR="${HOME}/Applications"
CONFIG_DIR="${HOME}/.openflow"
CONFIG_PATH="${CONFIG_DIR}/config.json"

if [ ! -d "${APP_DIR}" ]; then
  echo "error: ${APP_DIR} not found. Run scripts/build_app.sh first."
  exit 1
fi

mkdir -p "${TARGET_DIR}"
rm -rf "${TARGET_DIR}/${APP_NAME}.app"
cp -R "${APP_DIR}" "${TARGET_DIR}/"
rm -rf "${APP_DIR}"

if [ ! -f "${CONFIG_PATH}" ]; then
  mkdir -p "${CONFIG_DIR}"
  cp "${ROOT_DIR}/default_config.json" "${CONFIG_PATH}"
  echo "✅ Initialized ${CONFIG_PATH} from default_config.json"
fi

echo "✅ Installed to ${TARGET_DIR}/${APP_NAME}.app"
