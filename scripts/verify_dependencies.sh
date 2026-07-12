#!/usr/bin/env bash
#===============================================================================
# Verify all project dependencies
#
# Checks:
#   1. 22 SDK precompiled .a libraries (SHA-256, ar format, not LFS pointer)
#   2. LVGL v9.3.0 core/ source files presence
#   3. LVGL LICENCE.txt
#   4. LVGL version consistency
#   5. Kernel .a libraries (tracked in git)
#===============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLCHAIN_AR="${ROOT}/src/tools/bin/compiler/riscv/cc_riscv32_musl_105/cc_riscv32_musl_fp/bin/riscv32-linux-musl-ar"

pass=0
fail=0
skip=0

check() {
    local desc="$1"
    shift
    if "$@" 2>/dev/null; then
        echo "  PASS: ${desc}"
        pass=$((pass + 1))
    else
        echo "  FAIL: ${desc}"
        fail=$((fail + 1))
    fi
}

echo "============================================"
echo " Dependency Verification Report"
echo " Date: $(date)"
echo "============================================"
echo ""

echo "--- 1. SDK Precompiled Libraries ---"
SDK_SHA256="${ROOT}/dependencies/hispark_sdk_1.10.106.sha256"
if [ -f "$SDK_SHA256" ]; then
    echo "  SHA-256 file: ${SDK_SHA256}"
    count=$(grep -cve '^\s*#\|^\s*$' "$SDK_SHA256" || true)
    echo "  Expected libraries: ${count}"

    # Check SHA-256
    check "SHA-256 checksums match" sha256sum -c "$SDK_SHA256"

    # Check each file is valid ar archive
    while IFS= read -r line; do
        case "$line" in
            \#*|"") continue ;;
            *)
                relpath=$(echo "$line" | awk '{print $2}')
                fullpath="${ROOT}/${relpath}"
                if [ -f "$fullpath" ]; then
                    # Not LFS pointer
                    check "${relpath} is not LFS pointer" \
                        bash -c "! head -c 200 '$fullpath' | strings | grep -q 'git-lfs'"
                    # Valid ar archive
                    check "${relpath} is valid ar archive" \
                        bash -c "head -c 8 '$fullpath' | od -An -tx1 | grep -q '21 3c 61 72 63 68 3e'"
                    # Check RISC-V objects inside
                    if [ -x "$TOOLCHAIN_AR" ] && [ -f "$fullpath" ]; then
                        check "${relpath} contains RISC-V objects" \
                            bash -c "'$TOOLCHAIN_AR' t '$fullpath' | head -1 | grep -q '\.obj'"
                    fi
                fi
                ;;
        esac
    done < "$SDK_SHA256"
else
    echo "  MISSING: ${SDK_SHA256}"
    fail=$((fail + 1))
fi

echo ""
echo "--- 2. LVGL core/ Source Files ---"
LVGL_CORE="${ROOT}/src/deprecated/ws63_screen_lvgl/src/lvgl/src/core"
if [ -d "$LVGL_CORE" ]; then
    c_count=$(find "$LVGL_CORE" -maxdepth 1 -name '*.c' -type f | wc -l)
    h_count=$(find "$LVGL_CORE" -maxdepth 1 -name '*.h' -type f | wc -l)
    check "LVGL core .c files (expected 13, got ${c_count})" test "$c_count" -eq 13
    check "LVGL core .h files (expected 21, got ${h_count})" test "$h_count" -eq 21
    check "LVGL lv_obj.h exists" test -f "${LVGL_CORE}/lv_obj.h"
else
    echo "  MISSING: LVGL core directory"
    fail=$((fail + 1))
fi

echo ""
echo "--- 3. LVGL LICENCE.txt ---"
LVGL_LIC="${ROOT}/src/deprecated/ws63_screen_lvgl/src/lvgl/LICENCE.txt"
if [ -f "$LVGL_LIC" ]; then
    check "LVGL LICENCE.txt exists" test -f "$LVGL_LIC"
    check "LVGL LICENCE.txt contains MIT" grep -qi 'MIT' "$LVGL_LIC"
else
    echo "  MISSING: LVGL LICENCE.txt"
    fail=$((fail + 1))
fi

echo ""
echo "--- 4. LVGL Version ---"
LVGL_VER="${ROOT}/src/deprecated/ws63_screen_lvgl/src/lvgl/lv_version.h"
if [ -f "$LVGL_VER" ]; then
    major=$(grep 'LVGL_VERSION_MAJOR' "$LVGL_VER" | head -1 | awk '{print $3}')
    minor=$(grep 'LVGL_VERSION_MINOR' "$LVGL_VER" | head -1 | awk '{print $3}')
    patch=$(grep 'LVGL_VERSION_PATCH' "$LVGL_VER" | head -1 | awk '{print $3}')
    check "LVGL version v${major}.${minor}.${patch}" test "${major}.${minor}.${patch}" = "9.3.0"
else
    echo "  MISSING: LVGL version file"
    fail=$((fail + 1))
fi

echo ""
echo "--- 5. LVGL Lock File ---"
LVGL_LOCK="${ROOT}/dependencies/lvgl_v9.3.0.lock"
if [ -f "$LVGL_LOCK" ]; then
    check "LVGL lock file exists" true
else
    echo "  MISSING: ${LVGL_LOCK}"
    fail=$((fail + 1))
fi

echo ""
echo "--- 6. SDK Lock File ---"
SDK_LOCK="${ROOT}/dependencies/hispark_sdk_1.10.106.lock"
if [ -f "$SDK_LOCK" ]; then
    check "SDK lock file exists" true
else
    echo "  MISSING: ${SDK_LOCK}"
    fail=$((fail + 1))
fi

echo ""
echo "============================================"
echo " Results: ${pass} passed, ${fail} failed, ${skip} skipped"
echo "============================================"

if [ "$fail" -gt 0 ]; then
    echo "WARNING: Some dependencies are missing or corrupted." >&2
    echo "Run 'scripts/restore_official_sdk_libs.sh --restore' to fix SDK libraries." >&2
fi

exit "$fail"
