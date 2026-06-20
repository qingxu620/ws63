#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# WS63 Screen Firmware Build & Archive Script
# =============================================================================
# Builds the screen node firmware. Current hardware selection is MSP3223
# (ILI9341V LCD + FT6336U touch). The raw self-test module still contains
# historical ST7796 bring-up code until the MSP3223/ILI9341V port lands.
#
# Usage:
#   ./scripts/build_screen_firmware.sh            # default: LVGL
#   ./scripts/build_screen_firmware.sh --lvgl     # explicit LVGL
#   ./scripts/build_screen_firmware.sh --selftest  # raw self-test page

ROOT="/root/fbb_ws63"
CONFIG="${ROOT}/src/build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config"
FWPKG_DIR="${ROOT}/src/output/ws63/fwpkg/ws63-liteos-app"
FWPKG_FILE="ws63-liteos-app_all.fwpkg"
STAGE_DIR="${ROOT}/src/output/ws63/fwstage"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
BUILD_CMD="python3 build.py -c ws63-liteos-app -ninja -j24"
DEST_NAME="ws63-liteos-app_screen_all.fwpkg"

# --- Argument parsing --------------------------------------------------------

SCREEN_VARIANT="lvgl"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --lvgl)     SCREEN_VARIANT="lvgl";     shift ;;
        --selftest) SCREEN_VARIANT="selftest"; shift ;;
        -h|--help)
            cat <<EOF
Usage: $0 [--lvgl|--selftest]

Options:
  --lvgl      Build LVGL v9.3.0 port (default)
  --selftest  Build raw screen self-test page (currently historical ST7796 code)

Output:
  ${STAGE_DIR}/latest/${DEST_NAME}
EOF
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# --- Helpers -----------------------------------------------------------------

log() { echo "=== $* ==="; }
err() { echo "ERROR: $*" >&2; exit 1; }

find_build_py() {
    if [[ -f "${ROOT}/build.py" ]]; then echo "${ROOT}/build.py"
    elif [[ -f "${ROOT}/src/build.py" ]]; then echo "${ROOT}/src/build.py"
    else err "build.py not found"
    fi
}

set_config_y() {
    local symbol=$1
    if grep -q "^${symbol}=y$" "$CONFIG"; then return; fi
    if grep -q "^# ${symbol} is not set$" "$CONFIG"; then
        sed -i "s/^# ${symbol} is not set$/${symbol}=y/" "$CONFIG"; return
    fi
    if grep -q "^${symbol}=" "$CONFIG"; then
        sed -i "s/^${symbol}=.*/${symbol}=y/" "$CONFIG"; return
    fi
    printf '%s=y\n' "$symbol" >> "$CONFIG"
}

set_config_n() {
    local symbol=$1
    if grep -q "^# ${symbol} is not set$" "$CONFIG"; then return; fi
    if grep -q "^${symbol}=y$" "$CONFIG"; then
        sed -i "s/^${symbol}=y$/# ${symbol} is not set/" "$CONFIG"; return
    fi
    if grep -q "^${symbol}=" "$CONFIG"; then
        sed -i "s/^${symbol}=.*/# ${symbol} is not set/" "$CONFIG"; return
    fi
    printf '# %s is not set\n' "$symbol" >> "$CONFIG"
}

# Disable ALL competing app samples so only one is active per build.
disable_all_samples() {
    set_config_n CONFIG_ENABLE_LASER_SINGLE_SAMPLE
    set_config_n CONFIG_ENABLE_LASER_WIFI_SAMPLE
    set_config_n CONFIG_ENABLE_LASER_SLE_JOB_SAMPLE
    set_config_n CONFIG_ENABLE_SCREEN_SAMPLE
    set_config_n CONFIG_ENABLE_LVGL_SAMPLE
    set_config_n CONFIG_LASER_RX_UNIFIED
}

# --- Config switching --------------------------------------------------------

switch_config() {
    log "Switching config to screen (${SCREEN_VARIANT})"
    disable_all_samples

    case "$SCREEN_VARIANT" in
        lvgl)
            set_config_y CONFIG_ENABLE_LVGL_SAMPLE
            ;;
        selftest)
            set_config_y CONFIG_ENABLE_SCREEN_SAMPLE
            ;;
    esac

    verify_config
}

verify_config() {
    log "Verifying screen config"

    local conflicting=(
        CONFIG_ENABLE_LASER_SINGLE_SAMPLE
        CONFIG_ENABLE_LASER_WIFI_SAMPLE
        CONFIG_ENABLE_LASER_SLE_JOB_SAMPLE
        CONFIG_LASER_RX_UNIFIED
    )
    local symbol
    for symbol in "${conflicting[@]}"; do
        if grep -q "^${symbol}=y$" "$CONFIG"; then
            err "Conflicting config ${symbol}=y must be disabled"
        fi
    done

    case "$SCREEN_VARIANT" in
        lvgl)
            if ! grep -q '^CONFIG_ENABLE_LVGL_SAMPLE=y$' "$CONFIG"; then
                err "CONFIG_ENABLE_LVGL_SAMPLE=y not set"
            fi
            echo "  Screen config OK: ENABLE_LVGL_SAMPLE=y"
            ;;
        selftest)
            if ! grep -q '^CONFIG_ENABLE_SCREEN_SAMPLE=y$' "$CONFIG"; then
                err "CONFIG_ENABLE_SCREEN_SAMPLE=y not set"
            fi
            echo "  Screen config OK: ENABLE_SCREEN_SAMPLE=y"
            ;;
    esac
}

# --- Build & archive ---------------------------------------------------------

do_build() {
    log "Building screen firmware (${SCREEN_VARIANT})"

    local build_py build_dir
    build_py=$(find_build_py)
    build_dir=$(dirname "$build_py")

    cd "$build_dir"
    echo "  Build command: $BUILD_CMD"
    eval "$BUILD_CMD"

    local fwpkg="${FWPKG_DIR}/${FWPKG_FILE}"
    if [[ ! -f "$fwpkg" ]]; then
        err "Firmware not found after build: $fwpkg"
    fi
    echo "  Build output: $fwpkg ($(du -h "$fwpkg" | cut -f1))"
}

archive() {
    local ts_dir="${STAGE_DIR}/${TIMESTAMP}"
    local latest_dir="${STAGE_DIR}/latest"
    local src="${FWPKG_DIR}/${FWPKG_FILE}"

    mkdir -p "$ts_dir" "$latest_dir"

    cp "$src" "${ts_dir}/${DEST_NAME}"
    cp "$src" "${latest_dir}/${DEST_NAME}"

    echo "  Archived: ${ts_dir}/${DEST_NAME}"
    echo "  Archived: ${latest_dir}/${DEST_NAME}"
}

# --- Main --------------------------------------------------------------------

log "WS63 Screen Firmware Build"
echo "  Variant:   ${SCREEN_VARIANT}"
echo "  Config:    ${CONFIG}"
echo "  Timestamp: ${TIMESTAMP}"
echo ""

switch_config
do_build
archive

echo ""
log "Build complete"
echo ""
echo "Screen firmware:"
echo "  ${STAGE_DIR}/latest/${DEST_NAME}"
echo ""
echo "Use BurnTool on Windows to flash this package manually."
