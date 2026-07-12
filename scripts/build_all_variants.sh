#!/usr/bin/env bash
#===============================================================================
# Build all three firmware variants sequentially
#
# Usage:
#   scripts/build_all_variants.sh               # stop on first failure
#   scripts/build_all_variants.sh --continue     # continue on error
#===============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONTINUE_ON_ERROR=false

if [ "${1:-}" = "--continue" ]; then
    CONTINUE_ON_ERROR=true
fi

VARIANTS="tx rx_unified screen_panel"
RESULTS=""
ALL_OK=true

echo "=============================================="
echo " WS63 All Variants Builder"
echo " Date: $(date)"
echo "=============================================="
echo ""

for variant in $VARIANTS; do
    echo ">>> Building ${variant}..."
    echo ""
    if $CONTINUE_ON_ERROR; then
        set +e
        "${ROOT}/scripts/build_variant.sh" "$variant" 2>&1
        rc=$?
        set -e
    else
        "${ROOT}/scripts/build_variant.sh" "$variant"
        rc=$?
    fi

    if [ "$rc" -eq 0 ]; then
        RESULTS="${RESULTS}  ${variant}: PASS\n"
    else
        RESULTS="${RESULTS}  ${variant}: FAIL (exit ${rc})\n"
        ALL_OK=false
    fi
    echo ""
done

echo "=============================================="
echo " Build Summary"
echo "=============================================="
echo -e "$RESULTS"

if $ALL_OK; then
    echo ""
    echo "All variants built successfully."
    echo ""
    echo "Archives:"
    for variant in $VARIANTS; do
        echo "  ${ROOT}/src/output/ws63/fwstage/${variant}/latest/"
    done
    exit 0
else
    echo ""
    echo "Some variants failed. See logs above."
    exit 1
fi
