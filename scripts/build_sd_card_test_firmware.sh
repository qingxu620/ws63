#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# WS63 MSP3223 SD-card Test Firmware Build & Archive Script
# =============================================================================

ROOT="/root/fbb_ws63"
CONFIG="${ROOT}/src/build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config"
FWPKG_DIR="${ROOT}/src/output/ws63/fwpkg/ws63-liteos-app"
FWPKG_FILE="ws63-liteos-app_all.fwpkg"
STAGE_DIR="${ROOT}/src/output/ws63/fwstage"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
BUILD_CMD="python3 build.py -c ws63-liteos-app -ninja -j24"
DEST_NAME="ws63-liteos-app_sd_test_all.fwpkg"

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

disable_all_samples() {
    set_config_n CONFIG_ENABLE_PERIPHERAL_SAMPLE
    set_config_n CONFIG_ENABLE_ALL_PERIPHERAL_SAMPLE
    set_config_n CONFIG_SAMPLE_SUPPORT_PWM
    set_config_n CONFIG_ENABLE_LASER_SINGLE_SAMPLE
    set_config_n CONFIG_ENABLE_LASER_WIFI_SAMPLE
    set_config_n CONFIG_ENABLE_LASER_SLE_JOB_SAMPLE
    set_config_n CONFIG_LASER_RX_UNIFIED
    set_config_n CONFIG_ENABLE_SCREEN_SAMPLE
    set_config_n CONFIG_ENABLE_LVGL_SAMPLE
    set_config_n CONFIG_ENABLE_LVGL_PANEL
    set_config_n CONFIG_ENABLE_SD_CARD_TEST
}

switch_config() {
    log "Switching config to standalone SD-card test"
    disable_all_samples
    set_config_y CONFIG_ENABLE_SD_CARD_TEST
    set_config_n CONFIG_SPI_SUPPORT_DMA
    set_config_n CONFIG_SPI_SUPPORT_POLL_AND_DMA_AUTO_SWITCH

    if ! grep -q '^CONFIG_ENABLE_SD_CARD_TEST=y$' "$CONFIG"; then
        err "CONFIG_ENABLE_SD_CARD_TEST=y not set"
    fi
    echo "  SD test config OK: ENABLE_SD_CARD_TEST=y"
}

do_build() {
    log "Building SD-card test firmware"

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

log "WS63 SD-card Test Firmware Build"
echo "  Config:    ${CONFIG}"
echo "  Timestamp: ${TIMESTAMP}"
echo ""

switch_config
do_build
archive

echo ""
log "Build complete"
echo ""
echo "SD-card test firmware:"
echo "  ${STAGE_DIR}/latest/${DEST_NAME}"
echo ""
echo "Use BurnTool on Windows to flash this package manually."
