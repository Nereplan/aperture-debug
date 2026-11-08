#!/usr/bin/env bash
# Native Linux release packaging script for CachyOS / Arch Linux
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="$(python3 -c "import json; print(json.load(open('${SCRIPT_DIR}/version.json'))['versionString'])")"

"${SCRIPT_DIR}/build_linux.sh" Release

STAGE_DIR="${SCRIPT_DIR}/dist/linux-x86_64"
mkdir -p "${STAGE_DIR}/data"
cp -f "${SCRIPT_DIR}/packaging/linux/Update.sh" "${STAGE_DIR}/Update.sh"
chmod +x "${STAGE_DIR}/Update.sh"
cp -f "${SCRIPT_DIR}/packaging/linux/aperture.desktop" "${STAGE_DIR}/aperture.desktop"
[ -f "${SCRIPT_DIR}/README.md" ] && cp -f "${SCRIPT_DIR}/README.md" "${STAGE_DIR}/README.md"
[ -f "${SCRIPT_DIR}/LICENSE" ] && cp -f "${SCRIPT_DIR}/LICENSE" "${STAGE_DIR}/LICENSE"
[ -f "${SCRIPT_DIR}/THIRD_PARTY_NOTICES.txt" ] && cp -f "${SCRIPT_DIR}/THIRD_PARTY_NOTICES.txt" "${STAGE_DIR}/THIRD_PARTY_NOTICES.txt"

if [ -d "${SCRIPT_DIR}/fonts" ]; then
    rm -rf "${STAGE_DIR}/fonts"
    cp -r "${SCRIPT_DIR}/fonts" "${STAGE_DIR}/fonts"
fi

if [ -d "${SCRIPT_DIR}/locales" ]; then
    rm -rf "${STAGE_DIR}/locales"
    cp -r "${SCRIPT_DIR}/locales" "${STAGE_DIR}/locales"
fi

ARCHIVE_NAME="Aperture-${VERSION}-linux-x86_64.tar.gz"
ARCHIVE_PATH="${SCRIPT_DIR}/dist/${ARCHIVE_NAME}"
rm -f "${ARCHIVE_PATH}" "${ARCHIVE_PATH}.sha256"

tar -czf "${ARCHIVE_PATH}" -C "${SCRIPT_DIR}/dist" "linux-x86_64"
(cd "${SCRIPT_DIR}/dist" && sha256sum "${ARCHIVE_NAME}" > "${ARCHIVE_NAME}.sha256")

echo "================================================================================"
echo "  Linux Portable Release Package Ready"
echo "  Archive: ${ARCHIVE_PATH}"
echo "  SHA-256: $(cat "${ARCHIVE_PATH}.sha256")"
echo "================================================================================"
