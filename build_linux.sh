#!/usr/bin/env bash
# Native Linux build script for CachyOS / Arch Linux
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_TYPE="${1:-Release}"
BUILD_DIR="${SCRIPT_DIR}/build/linux-${BUILD_TYPE}"

echo "================================================================================"
echo "  Aperture Linux Native Build (${BUILD_TYPE})"
echo "================================================================================"

GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
fi

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    "${GENERATOR_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}" -j"$(nproc 2>/dev/null || echo 4)"

mkdir -p "${SCRIPT_DIR}/dist/linux-x86_64"
cp -f "${BUILD_DIR}/aperture" "${SCRIPT_DIR}/dist/linux-x86_64/aperture"
chmod +x "${SCRIPT_DIR}/dist/linux-x86_64/aperture"

echo "Built Linux executable: ${SCRIPT_DIR}/dist/linux-x86_64/aperture"
