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
        echo "Timed out waiting for running Aperture process ($PARENT_PID) to exit." >&2
        exit 1
    fi
    sleep 0.1
done

CACHE_TMP="$TARGET_DIR/cache/updater-tmp"
ROLLBACK_DIR="$CACHE_TMP/rollback_$$"
TMP_EXTRACT="$CACHE_TMP/extract_$$"
mkdir -p "$ROLLBACK_DIR"

cleanup() {
    rm -rf "$TMP_EXTRACT" "$ROLLBACK_DIR" 2>/dev/null || true
}

rollback() {
    echo "Update failed; restoring previous Aperture installation..." >&2
    if [ -f "$ROLLBACK_DIR/aperture" ]; then
        cp -f "$ROLLBACK_DIR/aperture" "$TARGET_DIR/aperture" || true
        chmod +x "$TARGET_DIR/aperture" 2>/dev/null || true
    fi
    for sub in fonts locales; do
        if [ -d "$ROLLBACK_DIR/$sub" ]; then
            rm -rf "$TARGET_DIR/$sub" 2>/dev/null || true
            cp -rf "$ROLLBACK_DIR/$sub" "$TARGET_DIR/$sub" || true
        fi
    done
    cleanup
    if [ -x "$TARGET_DIR/aperture" ]; then
        nohup "$TARGET_DIR/aperture" >/dev/null 2>&1 &
    fi
}

# Snapshot existing runtime files (user data directory stays completely untouched)
if [ -f "$TARGET_DIR/aperture" ]; then
    cp -f "$TARGET_DIR/aperture" "$ROLLBACK_DIR/aperture"
fi
for sub in fonts locales; do
    if [ -d "$TARGET_DIR/$sub" ]; then
        cp -rf "$TARGET_DIR/$sub" "$ROLLBACK_DIR/$sub"
    fi
done

apply_ok=0
case "$STAGED_PATH" in
    *.tar.gz|*.tgz)
        rm -rf "$TMP_EXTRACT"
        mkdir -p "$TMP_EXTRACT"
        if tar -xzf "$STAGED_PATH" -C "$TMP_EXTRACT"; then
            SRC_ROOT=""
            if [ -f "$TMP_EXTRACT/aperture" ]; then
                SRC_ROOT="$TMP_EXTRACT"
            else
                SUBDIR=$(find "$TMP_EXTRACT" -mindepth 1 -maxdepth 1 -type d | head -n 1 || true)
                if [ -n "$SUBDIR" ] && [ -f "$SUBDIR/aperture" ]; then
                    SRC_ROOT="$SUBDIR"
                fi
            fi
            if [ -n "$SRC_ROOT" ]; then
                # Preserve existing data/ directory even if archive does not carry one
                cp_err=0
                for item in "$SRC_ROOT"/*; do
                    base=$(basename "$item")
                    if [ "$base" = "data" ] || [ "$base" = "cache" ]; then
                        continue
                    fi
                    if ! cp -rf "$item" "$TARGET_DIR/"; then
                        cp_err=1
                        break
                    fi
                done
                if [ "$cp_err" -eq 0 ]; then
                    apply_ok=1
                fi
            fi
        fi
        ;;
    *)
        if cp -f "$STAGED_PATH" "$TARGET_DIR/aperture.new" && \
           chmod +x "$TARGET_DIR/aperture.new" && \
           mv -f "$TARGET_DIR/aperture.new" "$TARGET_DIR/aperture"; then
            apply_ok=1
        fi
        ;;
esac

if [ "$apply_ok" -ne 1 ] || [ ! -f "$TARGET_DIR/aperture" ]; then
    rollback
    exit 1
fi

chmod +x "$TARGET_DIR/aperture"
cleanup
rm -f "$STAGED_PATH" 2>/dev/null || true

# Relaunch updated Aperture in the background
nohup "$TARGET_DIR/aperture" >/dev/null 2>&1 &
exit 0
