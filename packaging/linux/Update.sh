#!/usr/bin/env sh
# Aperture Linux Portable Self-Updater
# Usage: Update.sh <parent_pid> <staged_payload_path> <target_dir>

set -eu

PARENT_PID="${1:-}"
STAGED_PATH="${2:-}"
TARGET_DIR="${3:-}"

if [ -z "$PARENT_PID" ] || [ -z "$STAGED_PATH" ] || [ -z "$TARGET_DIR" ]; then
    echo "Usage: Update.sh <parent_pid> <staged_payload_path> <target_dir>" >&2
    exit 1
fi

if [ ! -f "$STAGED_PATH" ]; then
    echo "Staged update payload not found: $STAGED_PATH" >&2
    exit 1
fi

# Wait up to 30 seconds for the running Aperture process to exit cleanly
ATTEMPTS=0
while kill -0 "$PARENT_PID" 2>/dev/null; do
    ATTEMPTS=$((ATTEMPTS + 1))
    if [ "$ATTEMPTS" -ge 300 ]; then
        break
    fi
    sleep 0.1
done

ROLLBACK_DIR="$TARGET_DIR/data/updates/rollback"
mkdir -p "$ROLLBACK_DIR"

if [ -f "$TARGET_DIR/aperture" ]; then
    cp -f "$TARGET_DIR/aperture" "$ROLLBACK_DIR/aperture.bak" || true
fi

case "$STAGED_PATH" in
    *.tar.gz|*.tgz)
        TMP_EXTRACT="$TARGET_DIR/data/updates/extract_$$"
        rm -rf "$TMP_EXTRACT"
        mkdir -p "$TMP_EXTRACT"
        tar -xzf "$STAGED_PATH" -C "$TMP_EXTRACT"
        if [ -f "$TMP_EXTRACT/aperture" ]; then
            cp -rf "$TMP_EXTRACT"/* "$TARGET_DIR/"
        else
            SUBDIR=$(find "$TMP_EXTRACT" -mindepth 1 -maxdepth 1 -type d | head -n 1)
            if [ -n "$SUBDIR" ] && [ -f "$SUBDIR/aperture" ]; then
                cp -rf "$SUBDIR"/* "$TARGET_DIR/"
            fi
        fi
        rm -rf "$TMP_EXTRACT"
        ;;
    *)
        cp -f "$STAGED_PATH" "$TARGET_DIR/aperture.new"
        chmod +x "$TARGET_DIR/aperture.new"
        mv -f "$TARGET_DIR/aperture.new" "$TARGET_DIR/aperture"
        ;;
esac

chmod +x "$TARGET_DIR/aperture"

# Relaunch updated Aperture in the background
nohup "$TARGET_DIR/aperture" >/dev/null 2>&1 &
exit 0
