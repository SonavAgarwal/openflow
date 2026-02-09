#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APP_NAME="OpenFlow"
APP_DIR="${ROOT_DIR}/dist/${APP_NAME}.app"
CONTENTS_DIR="${APP_DIR}/Contents"
MACOS_DIR="${CONTENTS_DIR}/MacOS"
RESOURCES_DIR="${CONTENTS_DIR}/Resources"

TRANSCRIBER_DIR="${ROOT_DIR}/transcriber"
TRANSCRIBER_BUILD_BIN="${TRANSCRIBER_DIR}/build/bin"
WHISPER_MODELS_DIR="${TRANSCRIBER_DIR}/whisper.cpp/models"

SMALL_MODEL="${WHISPER_MODELS_DIR}/ggml-small.en.bin"
SILERO_MODEL="${WHISPER_MODELS_DIR}/ggml-silero-v5.1.2.bin"

INFO_PLIST="${CONTENTS_DIR}/Info.plist"
APP_ICON_PNG="${ROOT_DIR}/assets/openflow_circle_1024.png"

echo "==> Building Swift app (release)"
cd "${ROOT_DIR}"
swift build -c release

if [ ! -x "${TRANSCRIBER_BUILD_BIN}/openflow_transcriber" ]; then
  echo "==> Building transcriber + VAD"
  cd "${TRANSCRIBER_DIR}"
  ./scripts/setup_whisper.sh
fi

if [ ! -f "${SMALL_MODEL}" ]; then
  echo "error: missing whisper model at ${SMALL_MODEL}"
  exit 1
fi

if [ ! -f "${SILERO_MODEL}" ]; then
  echo "error: missing silero VAD model at ${SILERO_MODEL}"
  exit 1
fi

SWIFT_BIN="${ROOT_DIR}/.build/release/openflow"
if [ ! -x "${SWIFT_BIN}" ]; then
  echo "error: swift binary not found at ${SWIFT_BIN}"
  exit 1
fi

rm -rf "${APP_DIR}"
mkdir -p "${MACOS_DIR}" "${RESOURCES_DIR}"

cat > "${INFO_PLIST}" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>
  <string>${APP_NAME}</string>
  <key>CFBundleDisplayName</key>
  <string>${APP_NAME}</string>
  <key>CFBundleIconFile</key>
  <string>AppIcon</string>
  <key>CFBundleIdentifier</key>
  <string>com.openflow.app</string>
  <key>CFBundleVersion</key>
  <string>1</string>
  <key>CFBundleShortVersionString</key>
  <string>0.1.0</string>
  <key>CFBundleExecutable</key>
  <string>${APP_NAME}</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>LSMinimumSystemVersion</key>
  <string>13.0</string>
  <key>NSMicrophoneUsageDescription</key>
  <string>OpenFlow needs microphone access to transcribe speech.</string>
  <key>NSHumanReadableCopyright</key>
  <string>Copyright 2026 OpenFlow</string>
</dict>
</plist>
PLIST

cp "${SWIFT_BIN}" "${MACOS_DIR}/${APP_NAME}"
chmod +x "${MACOS_DIR}/${APP_NAME}"

if [ -f "${APP_ICON_PNG}" ]; then
  cp "${APP_ICON_PNG}" "${RESOURCES_DIR}/AppIcon.png"
fi

# Bundle transcriber binaries + models
mkdir -p "${RESOURCES_DIR}/transcriber/build/bin"
mkdir -p "${RESOURCES_DIR}/transcriber/whisper.cpp/models"
cp "${TRANSCRIBER_BUILD_BIN}/openflow_transcriber" "${RESOURCES_DIR}/transcriber/build/bin/"
cp "${TRANSCRIBER_BUILD_BIN}/transcriber" "${RESOURCES_DIR}/transcriber/build/bin/"
cp "${SMALL_MODEL}" "${RESOURCES_DIR}/transcriber/whisper.cpp/models/"
cp "${SILERO_MODEL}" "${RESOURCES_DIR}/transcriber/whisper.cpp/models/"

echo "✅ Built ${APP_DIR}"
