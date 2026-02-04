#!/usr/bin/env bash
set -euo pipefail

# Go to repo root (assuming this script lives in transcriber/scripts)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TRANSCRIBER_DIR="${REPO_ROOT}/transcriber"
WHISPER_DIR="${TRANSCRIBER_DIR}/whisper.cpp"
PYTHON_BIN="${PYTHON_BIN:-python3}"

echo "==> Repo root: ${REPO_ROOT}"
echo "==> Transcriber dir: ${TRANSCRIBER_DIR}"
echo "==> Whisper.cpp dir: ${WHISPER_DIR}"

# Step 0: Init/update whisper.cpp (submodule if configured; otherwise clone)
if [ ! -d "${WHISPER_DIR}/.git" ] && [ ! -d "${WHISPER_DIR}/src" ]; then
  if git -C "${REPO_ROOT}" submodule status -- transcriber/whisper.cpp >/dev/null 2>&1; then
    echo "==> Initializing whisper.cpp submodule"
    git -C "${REPO_ROOT}" submodule update --init --recursive -- transcriber/whisper.cpp
  else
    echo "==> Cloning whisper.cpp (submodule not initialized)"
    git clone https://github.com/ggml-org/whisper.cpp "${WHISPER_DIR}"
  fi
else
  if [ -f "${WHISPER_DIR}/.git" ]; then
    echo "==> Updating whisper.cpp"
    git -C "${WHISPER_DIR}" fetch --all --prune
    git -C "${WHISPER_DIR}" checkout master >/dev/null 2>&1 || true
    git -C "${WHISPER_DIR}" pull --ff-only || true
  else
    echo "==> whisper.cpp folder already present"
  fi
fi

# Ensure tracked files inside the submodule are clean before applying patches.
# Note: don't `git clean` here, because whisper.cpp setup downloads models into untracked paths.
echo "==> Resetting whisper.cpp tracked files"
git -C "${WHISPER_DIR}" reset --hard

# Step 0.5: Apply local patches to whisper.cpp (idempotent)
PATCH_DIR="${SCRIPT_DIR}/patches"
WHISPER_PATCHES=(
  "${PATCH_DIR}/whispercpp-arm-repack-neon-guard.patch"
)

echo "==> Applying local whisper.cpp patches (if needed)"
for PATCH_FILE in "${WHISPER_PATCHES[@]}"; do
  if [ ! -f "${PATCH_FILE}" ]; then
    continue
  fi

  if git -C "${WHISPER_DIR}" apply --reverse --check "${PATCH_FILE}" >/dev/null 2>&1; then
    echo "==> Patch already applied: $(basename "${PATCH_FILE}")"
    continue
  fi

  echo "==> Applying patch: $(basename "${PATCH_FILE}")"
  if ! git -C "${WHISPER_DIR}" apply "${PATCH_FILE}"; then
    echo "==> Warning: patch failed to apply: $(basename "${PATCH_FILE}")"
    echo "==> Continuing without this patch."
  fi
done

# Step 1: Download ggml model (base.en)
echo "==> Downloading ggml model (base.en)"
bash "${WHISPER_DIR}/models/download-ggml-model.sh" base.en

# Step 2.5: Download small.en
echo "==> Downloading ggml model (small.en)"
bash "${WHISPER_DIR}/models/download-ggml-model.sh" small.en

# Step 2.75: Download Silero VAD ggml model
echo "==> Downloading Silero VAD ggml model (silero-v5.1.2)"
bash "${WHISPER_DIR}/models/download-vad-model.sh" silero-v5.1.2 "${WHISPER_DIR}/models"


# Step 3: Configure + build transcriber
echo "==> Running CMake configure + build"
BUILD_DIR="${TRANSCRIBER_DIR}/build"
rm -rf "${BUILD_DIR}"

CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=Release
  -DBUILD_SHARED_LIBS=OFF
  -DGGML_METAL=ON
  -DGGML_METAL_EMBED_LIBRARY=ON
  -DWHISPER_SDL2=ON
)

cmake -S "${TRANSCRIBER_DIR}" -B "${BUILD_DIR}" \
  "${CMAKE_ARGS[@]}"
cmake --build "${BUILD_DIR}" --target transcriber openflow_transcriber -j

echo
echo "✅ Setup complete!"
echo "Binary is at: ${BUILD_DIR}/bin/transcriber"
