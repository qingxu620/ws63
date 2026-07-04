#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# WS63 SLE TX + Unified RX Firmware Build & Archive Script
# =============================================================================

ROOT="/root/fbb_ws63"
CONFIG="${ROOT}/src/build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config"
FWPKG_DIR="${ROOT}/src/output/ws63/fwpkg/ws63-liteos-app"
FWPKG_FILE="ws63-liteos-app_all.fwpkg"
STAGE_DIR="${ROOT}/src/output/ws63/fwstage"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
BUILD_CMD="python3 build.py -c ws63-liteos-app -ninja -j24"

# --- Argument parsing --------------------------------------------------------

BUILD_TARGET="both"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --both)    BUILD_TARGET="both"; shift ;;
        --tx-only) BUILD_TARGET="tx";   shift ;;
        --rx-only)
            echo "ERROR: --rx-only is retired. RX now means unified RX; use scripts/build_rx_unified_firmware.sh" >&2
            exit 1
            ;;
        -h|--help)
            echo "Usage: $0 [--both|--tx-only]"
            echo "  --both     Build TX, then build unified RX via build_rx_unified_firmware.sh (default)"
            echo "  --tx-only  Build TX only"
            echo ""
            echo "RX now means ws63-liteos-app_rx_unified_all.fwpkg."
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# --- Helper functions --------------------------------------------------------

log()  { echo "=== $* ==="; }
err()  { echo "ERROR: $*" >&2; exit 1; }

# --- Config helpers (shared) -------------------------------------------------

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

set_config_int() {
    local symbol=$1
    local value=$2
    if grep -q "^${symbol}=" "$CONFIG"; then
        sed -i "s/^${symbol}=.*/${symbol}=${value}/" "$CONFIG"
        return
    fi
    if grep -q "^# ${symbol} is not set$" "$CONFIG"; then
        sed -i "s/^# ${symbol} is not set$/${symbol}=${value}/" "$CONFIG"
        return
    fi
    printf '%s=%s\n' "$symbol" "$value" >> "$CONFIG"
}

# Disable ALL competing app samples so only one is active per build.
disable_all_samples() {
    set_config_n CONFIG_ENABLE_LASER_SINGLE_SAMPLE
    set_config_n CONFIG_ENABLE_LASER_WIFI_SAMPLE
    set_config_n CONFIG_ENABLE_LASER_SLE_JOB_SAMPLE
    set_config_n CONFIG_ENABLE_SCREEN_SAMPLE
    set_config_n CONFIG_ENABLE_LVGL_SAMPLE
    set_config_n CONFIG_ENABLE_LVGL_PANEL
    set_config_n CONFIG_LASER_RX_UNIFIED
}

find_build_py() {
    if [[ -f "${ROOT}/build.py" ]]; then
        echo "${ROOT}/build.py"
    elif [[ -f "${ROOT}/src/build.py" ]]; then
        echo "${ROOT}/src/build.py"
    else
        err "build.py not found in ${ROOT}/ or ${ROOT}/src/"
    fi
}

switch_to_tx() {
    log "Switching config to TX"
    disable_all_samples
    set_config_y CONFIG_ENABLE_LASER_SLE_JOB_SAMPLE
    sed -i 's/^CONFIG_LASER_SLE_JOB_RECEIVER=y$/# CONFIG_LASER_SLE_JOB_RECEIVER is not set/' "$CONFIG"
    sed -i 's/^# CONFIG_LASER_SLE_JOB_TRANSMITTER is not set$/CONFIG_LASER_SLE_JOB_TRANSMITTER=y/' "$CONFIG"
    if ! grep -q '^CONFIG_LASER_SLE_JOB_TRANSMITTER=y$' "$CONFIG"; then
        sed -i '/^# CONFIG_LASER_SLE_JOB_RECEIVER is not set$/a CONFIG_LASER_SLE_JOB_TRANSMITTER=y' "$CONFIG"
    fi
    if ! grep -q '^CONFIG_LASER_SLE_JOB_TRANSMITTER=y$' "$CONFIG"; then
        sed -i '/^CONFIG_ENABLE_LASER_SLE_JOB_SAMPLE=y$/a CONFIG_LASER_SLE_JOB_TRANSMITTER=y' "$CONFIG"
    fi
    set_config_int CONFIG_UART1_BAUDRATE 115200
    set_config_int CONFIG_LOG_UART_BAUDRATE 115200
    set_config_int CONFIG_LASER_SLE_JOB_UART_BAUD 115200
    verify_tx_config
}

verify_tx_config() {
    log "Verifying TX config"
    local tx_on rx_on
    tx_on=$(grep -c '^CONFIG_LASER_SLE_JOB_TRANSMITTER=y$' "$CONFIG" || true)
    rx_on=$(grep -c '^CONFIG_LASER_SLE_JOB_RECEIVER=y$' "$CONFIG" || true)
    if [[ "$tx_on" -ne 1 ]]; then
        err "TX config verification failed: CONFIG_LASER_SLE_JOB_TRANSMITTER=y count=$tx_on (expected 1)"
    fi
    if [[ "$rx_on" -ne 0 ]]; then
        err "TX config verification failed: CONFIG_LASER_SLE_JOB_RECEIVER=y count=$rx_on (expected 0)"
    fi
    echo "  TX config OK: TRANSMITTER=y, RECEIVER=not set"
}

do_build() {
    local role=$1
    log "Building ${role^^} firmware"

    local build_py
    build_py=$(find_build_py)
    local build_dir
    build_dir=$(dirname "$build_py")

    cd "$build_dir"
    echo "  Build command: $BUILD_CMD"
    echo "  Build directory: $build_dir"
    eval "$BUILD_CMD"

    # Check output
    local fwpkg="${FWPKG_DIR}/${FWPKG_FILE}"
    if [[ ! -f "$fwpkg" ]]; then
        err "Firmware not found after build: $fwpkg"
    fi
    echo "  Build output: $fwpkg ($(du -h "$fwpkg" | cut -f1))"
}

archive() {
    local role=$1
    local ts_dir="${STAGE_DIR}/${TIMESTAMP}"
    local latest_dir="${STAGE_DIR}/latest"
    local dest_name
    local src="${FWPKG_DIR}/${FWPKG_FILE}"

    case "$role" in
        tx) dest_name="ws63-liteos-app_tx_all.fwpkg" ;;
        *) err "Unknown archive role: $role" ;;
    esac

    mkdir -p "$ts_dir" "$latest_dir"

    cp "$src" "${ts_dir}/${dest_name}"
    cp "$src" "${latest_dir}/${dest_name}"

    echo "  Archived: ${ts_dir}/${dest_name}"
    echo "  Archived: ${latest_dir}/${dest_name}"
}

generate_manifest() {
    local dir=$1
    local manifest="${dir}/manifest.txt"
    local git_hash git_status tx_fw unified_rx_fw

    git_hash=$(cd "$ROOT" && git rev-parse --short HEAD 2>/dev/null || echo "unknown")
    git_status=$(cd "$ROOT" && git status --porcelain 2>/dev/null | wc -l | tr -d ' ')

    tx_fw="${dir}/ws63-liteos-app_tx_all.fwpkg"
    unified_rx_fw="${STAGE_DIR}/latest/ws63-liteos-app_rx_unified_all.fwpkg"

    local diag_log chunk_max
    diag_log=$(grep '^#define JOB_DIAG_LOG ' "${ROOT}/src/ws63_laser_sle_tx/common/config.h" | awk '{print $3}' || echo "unknown")
    chunk_max=$(grep '^#define JOB_TX_DATA_CHUNK_MAX ' "${ROOT}/src/ws63_laser_sle_tx/common/config.h" | awk '{print $3}' || echo "unknown")

    cat > "$manifest" <<EOF
Build Time:       ${TIMESTAMP}
Git Commit:       ${git_hash}
Git Clean:        $([ "$git_status" -eq 0 ] && echo "yes" || echo "no (${git_status} modified files)")
Build Command:    ${BUILD_CMD}
JOB_DIAG_LOG:     ${diag_log}
JOB_TX_DATA_CHUNK_MAX: ${chunk_max}

TX Config:
  CONFIG_LASER_SLE_JOB_TRANSMITTER=y
  CONFIG_LASER_SLE_JOB_RECEIVER is not set

RX Naming:
  RX means unified RX only: ws63-liteos-app_rx_unified_all.fwpkg

TX Firmware:
  Path:   ${tx_fw}
EOF
    if [[ -f "$tx_fw" ]]; then
        cat >> "$manifest" <<EOF
  Size:   $(du -b "$tx_fw" | cut -f1) bytes ($(du -h "$tx_fw" | cut -f1))
  SHA256: $(sha256sum "$tx_fw" | cut -d' ' -f1)
EOF
    else
        echo "  (not built)" >> "$manifest"
    fi

    cat >> "$manifest" <<EOF

Unified RX Firmware:
  Path:   ${unified_rx_fw}
EOF
    if [[ -f "$unified_rx_fw" ]]; then
        cat >> "$manifest" <<EOF
  Size:   $(du -b "$unified_rx_fw" | cut -f1) bytes ($(du -h "$unified_rx_fw" | cut -f1))
  SHA256: $(sha256sum "$unified_rx_fw" | cut -d' ' -f1)
EOF
    else
        echo "  (not built by this manifest step; run scripts/build_rx_unified_firmware.sh)" >> "$manifest"
    fi

    echo "  Manifest: ${manifest}"
}

build_unified_rx() {
    log "Building unified RX firmware"
    "${ROOT}/scripts/build_rx_unified_firmware.sh"
}

# --- Main --------------------------------------------------------------------

log "WS63 SLE TX + Unified RX Firmware Build"
echo "  Target:    ${BUILD_TARGET}"
echo "  Config:    ${CONFIG}"
echo "  Timestamp: ${TIMESTAMP}"
echo ""

case "$BUILD_TARGET" in
    tx)
        switch_to_tx
        do_build tx
        archive tx
        generate_manifest "${STAGE_DIR}/${TIMESTAMP}"
        generate_manifest "${STAGE_DIR}/latest"
        ;;
    both)
        switch_to_tx
        do_build tx
        archive tx
        generate_manifest "${STAGE_DIR}/${TIMESTAMP}"
        generate_manifest "${STAGE_DIR}/latest"
        build_unified_rx
        generate_manifest "${STAGE_DIR}/latest"
        ;;
esac

echo ""
log "Build complete"
echo ""
echo "TX firmware:"
echo "  ${STAGE_DIR}/latest/ws63-liteos-app_tx_all.fwpkg"
echo ""
echo "RX firmware:"
echo "  ${STAGE_DIR}/latest/ws63-liteos-app_rx_unified_all.fwpkg"
echo ""
echo "Use BurnTool on Windows to flash these two packages manually."
