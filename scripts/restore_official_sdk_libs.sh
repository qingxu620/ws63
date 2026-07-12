#!/usr/bin/env bash
#===============================================================================
# Restore / verify HiSpark WS63 SDK 1.10.106 precompiled libraries
#
# Source: https://gitee.com/HiSpark/fbb_ws63.git  tag 1.10.106  (7cda10e7)
#
# Usage:
#   scripts/restore_official_sdk_libs.sh --verify-only
#   scripts/restore_official_sdk_libs.sh --restore
#   scripts/restore_official_sdk_libs.sh --help
#===============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OFFICIAL_REPO="https://gitee.com/HiSpark/fbb_ws63.git"
OFFICIAL_TAG="1.10.106"
OFFICIAL_COMMIT="7cda10e7476d8df35f0ac1a4013eb395874247e0"
SHA256_FILE="${ROOT}/dependencies/hispark_sdk_1.10.106.sha256"
TMPDIR=""

cleanup() {
    if [ -n "$TMPDIR" ] && [ -d "$TMPDIR" ]; then
        rm -rf "$TMPDIR" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM HUP

count_pass=0
count_skip=0
count_fail=0
count_missing=0

verify_one() {
    local relpath="$1"
    local fullpath="${ROOT}/${relpath}"

    if [ ! -f "$fullpath" ]; then
        echo "  MISSING: ${relpath}"
        count_missing=$((count_missing + 1))
        return 1
    fi

    # Check not a git LFS pointer
    if head -c 200 "$fullpath" | strings | grep -q 'git-lfs' 2>/dev/null; then
        echo "  LFS_POINTER: ${relpath}"
        count_fail=$((count_fail + 1))
        return 1
    fi

    # Check valid ar archive
    if ! head -c 8 "$fullpath" | od -An -tx1 | grep -q '21 3c 61 72 63 68 3e'; then
        echo "  NOT_AR: ${relpath}"
        count_fail=$((count_fail + 1))
        return 1
    fi

    # Verify SHA-256
    if sha256sum -c "$SHA256_FILE" 2>/dev/null | grep -q "^${relpath}: OK$"; then
        echo "  OK: ${relpath}"
        count_pass=$((count_pass + 1))
        return 0
    else
        echo "  HASH_MISMATCH: ${relpath}"
        count_fail=$((count_fail + 1))
        return 1
    fi
}

restore_one() {
    local relpath="$1"
    local fullpath="${ROOT}/${relpath}"

    # If exists and hash matches, skip
    if [ -f "$fullpath" ]; then
        if sha256sum -c "$SHA256_FILE" 2>/dev/null | grep -q "^${relpath}: OK$"; then
            echo "  SKIP (already correct): ${relpath}"
            count_skip=$((count_skip + 1))
            return 0
        else
            echo "  ERROR: ${relpath} exists but hash mismatch. Refusing to overwrite." >&2
            count_fail=$((count_fail + 1))
            return 1
        fi
    fi

    # Extract from official repo at matching tag
    echo "  RESTORE: ${relpath}"
    mkdir -p "$(dirname "$fullpath")"
    if ! git -C "$TMPDIR/repo" show "${OFFICIAL_COMMIT}:${relpath}" > "$fullpath" 2>/dev/null; then
        echo "  ERROR: failed to extract ${relpath} from official repo" >&2
        count_fail=$((count_fail + 1))
        return 1
    fi

    # Verify after extraction
    if sha256sum -c "$SHA256_FILE" 2>/dev/null | grep -q "^${relpath}: OK$"; then
        echo "  RESTORED OK: ${relpath}"
        count_pass=$((count_pass + 1))
        return 0
    else
        echo "  HASH_MISMATCH after restore: ${relpath}" >&2
        count_fail=$((count_fail + 1))
        return 1
    fi
}

do_verify_only() {
    echo "=== Verifying SDK libraries (SHA-256) ==="
    echo ""

    while IFS= read -r line; do
        case "$line" in
            \#*|"") continue ;;
            *)
                relpath=$(echo "$line" | awk '{print $2}')
                verify_one "$relpath" || true
                ;;
        esac
    done < "$SHA256_FILE"

    echo ""
    echo "=== Summary: ${count_pass} OK, ${count_skip} skipped, ${count_fail} failed, ${count_missing} missing ==="
    [ "$count_fail" -eq 0 ] && [ "$count_missing" -eq 0 ]
}

do_restore() {
    echo "=== Restoring SDK libraries from official repo ==="
    echo "  Repo: ${OFFICIAL_REPO}"
    echo "  Tag:  ${OFFICIAL_TAG}"
    echo ""

    # Clone official repo to temp directory
    TMPDIR=$(mktemp -d)
    echo "  Cloning official repo..."
    git clone --branch "$OFFICIAL_TAG" --depth 1 "$OFFICIAL_REPO" "$TMPDIR/repo" 2>/dev/null || {
        git clone --depth 1 "$OFFICIAL_REPO" "$TMPDIR/repo" 2>/dev/null
    }

    # Verify commit
    CLONED_COMMIT=$(git -C "$TMPDIR/repo" rev-parse HEAD 2>/dev/null || echo "")
    if [ "$CLONED_COMMIT" != "$OFFICIAL_COMMIT" ]; then
        echo "  WARNING: Official repo HEAD (${CLONED_COMMIT}) differs from expected (${OFFICIAL_COMMIT})" >&2
    fi

    while IFS= read -r line; do
        case "$line" in
            \#*|"") continue ;;
            *)
                relpath=$(echo "$line" | awk '{print $2}')
                restore_one "$relpath" || true
                ;;
        esac
    done < "$SHA256_FILE"

    echo ""
    echo "=== Summary: ${count_pass} OK, ${count_skip} skipped, ${count_fail} failed, ${count_missing} missing ==="
    [ "$count_fail" -eq 0 ]
}

case "${1:-}" in
    --verify-only)
        do_verify_only
        ;;
    --restore)
        do_restore
        ;;
    --help|-h)
        echo "Usage: $0 [--verify-only|--restore]"
        echo ""
        echo "  --verify-only  Check SHA-256 of all 22 SDK libraries (safe, no network)"
        echo "  --restore      Download and restore missing/corrupted libraries from official repo"
        exit 0
        ;;
    *)
        echo "Usage: $0 [--verify-only|--restore]"
        echo "Try '$0 --help' for details."
        exit 1
        ;;
esac
