#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# WS63 Unified RX Firmware Build & Archive Script
# =============================================================================
if [ "${USE_LEGACY_BUILD:-}" != "true" ]; then
    echo "NOTE: $(basename "$0") is deprecated. Use scripts/build_variant.sh rx_unified instead." >&2
    scripts/build_variant.sh rx_unified
    exit $?
fi

ROOT="/root/fbb_ws63"
CONFIG="${ROOT}/src/build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config"
FWPKG_DIR="${ROOT}/src/output/ws63/fwpkg/ws63-liteos-app"
FWPKG_FILE="ws63-liteos-app_all.fwpkg"
STAGE_DIR="${ROOT}/src/output/ws63/fwstage"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
BUILD_CMD="python3 build.py -c ws63-liteos-app -ninja -j24"
DEST_NAME="ws63-liteos-app_rx_unified_all.fwpkg"

log() { echo "=== $* ==="; }
err() { echo "ERROR: $*" >&2; exit 1; }

usage() {
    cat <<EOF
Usage: $0

Build the unified RX sample and archive the firmware as:
  ${STAGE_DIR}/latest/ws63-liteos-app_rx_unified_all.fwpkg

The script switches ws63_liteos_app.config to CONFIG_LASER_RX_UNIFIED=y,
compiles all validated routes, enables the R5B persistent SLE advertising
policy, builds serially, and immediately
copies the shared raw fwpkg output into fwstage/latest and fwstage/<timestamp>.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi
if [[ $# -gt 0 ]]; then
    err "Unknown option: $1"
fi

find_build_py() {
    if [[ -f "${ROOT}/build.py" ]]; then
        echo "${ROOT}/build.py"
    elif [[ -f "${ROOT}/src/build.py" ]]; then
        echo "${ROOT}/src/build.py"
    else
        err "build.py not found in ${ROOT}/ or ${ROOT}/src/"
    fi
}

set_config_y() {
    local symbol=$1
    if grep -q "^${symbol}=y$" "$CONFIG"; then
        return
    fi
    if grep -q "^# ${symbol} is not set$" "$CONFIG"; then
        sed -i "s/^# ${symbol} is not set$/${symbol}=y/" "$CONFIG"
        return
    fi
    if grep -q "^${symbol}=" "$CONFIG"; then
        sed -i "s/^${symbol}=.*/${symbol}=y/" "$CONFIG"
        return
    fi
    printf '%s=y\n' "$symbol" >> "$CONFIG"
}

set_config_n() {
    local symbol=$1
    if grep -q "^# ${symbol} is not set$" "$CONFIG"; then
        return
    fi
    if grep -q "^${symbol}=y$" "$CONFIG"; then
        sed -i "s/^${symbol}=y$/# ${symbol} is not set/" "$CONFIG"
        return
    fi
    if grep -q "^${symbol}=" "$CONFIG"; then
        sed -i "s/^${symbol}=.*/# ${symbol} is not set/" "$CONFIG"
        return
    fi
    printf '# %s is not set\n' "$symbol" >> "$CONFIG"
}

set_config_int() {
    local symbol=$1
    local value=$2
    if grep -q "^${symbol}=" "$CONFIG"; then
        sed -i "s/^${symbol}=.*/${symbol}=${value}/" "$CONFIG"
    else
        printf '%s=%s\n' "$symbol" "$value" >> "$CONFIG"
    fi
}

assert_config_y() {
    local symbol=$1
    local count
    count=$(grep -c "^${symbol}=y$" "$CONFIG" || true)
    if [[ "$count" -ne 1 ]]; then
        err "${symbol}=y count=${count} (expected 1)"
    fi
}

assert_config_n() {
    local symbol=$1
    if grep -q "^${symbol}=y$" "$CONFIG"; then
        err "${symbol} must be disabled, but ${symbol}=y is present"
    fi
    if ! grep -q "^# ${symbol} is not set$" "$CONFIG"; then
        err "${symbol} disabled marker missing: # ${symbol} is not set"
    fi
}

assert_config_int() {
    local symbol=$1
    local value=$2
    local count
    count=$(grep -c "^${symbol}=${value}$" "$CONFIG" || true)
    if [[ "$count" -ne 1 ]]; then
        err "${symbol}=${value} count=${count} (expected 1)"
    fi
}

switch_to_rx_unified() {
    log "Switching config to unified RX"

    set_config_y CONFIG_SAMPLE_ENABLE

    # Keep only the unified RX app sample selected. The raw fwpkg output is
    # shared, so every function firmware must be built and archived serially.
    set_config_n CONFIG_ENABLE_BT_SAMPLE
    set_config_n CONFIG_ENABLE_PERIPHERAL_SAMPLE
    set_config_n CONFIG_ENABLE_WIFI_SAMPLE
    set_config_n CONFIG_ENABLE_PRODUCTS_SAMPLE
    set_config_n CONFIG_ENABLE_RADAR_SAMPLE
    set_config_n CONFIG_ENABLE_NFC_SAMPLE
    set_config_n CONFIG_ENABLE_LASER_SINGLE_SAMPLE
    set_config_n CONFIG_ENABLE_LASER_WIFI_SAMPLE
    set_config_n CONFIG_ENABLE_LASER_SLE_JOB_SAMPLE
    set_config_n CONFIG_ENABLE_SCREEN_SAMPLE
    set_config_n CONFIG_ENABLE_LVGL_SAMPLE
    set_config_n CONFIG_ENABLE_LVGL_PANEL
    set_config_y CONFIG_LASER_RX_UNIFIED

    # The standalone SLE TX sample is disabled; clear both old role selectors
    # so previous TX/legacy receiver builds cannot leak into unified RX config.
    set_config_n CONFIG_LASER_SLE_JOB_RECEIVER
    set_config_n CONFIG_LASER_SLE_JOB_TRANSMITTER

    # Compile all three routes for coverage, but the product boot policy starts
    # SLE Job only. Legacy WiFi coexist remains runtime-disabled by default.
    set_config_y CONFIG_LASER_RX_TRANSPORT_UART
    set_config_y CONFIG_LASER_RX_TRANSPORT_WIFI
    set_config_y CONFIG_LASER_RX_TRANSPORT_SLE_JOB
    set_config_int CONFIG_LASER_RX_SLE_JOB_CACHE_SIZE 102400
    set_config_n CONFIG_LASER_RX_SLE_WAIT_TIMEOUT_MS
    set_config_n CONFIG_LASER_RX_UART_STATUS_PERIODIC
    set_config_int CONFIG_LASER_RX_UART_BAUD 115200
    set_config_int CONFIG_LASER_RX_WORK_AREA_X_MM 99
    set_config_int CONFIG_LASER_RX_WORK_AREA_Y_MM 99

    verify_rx_unified_config
}

verify_rx_unified_config() {
    log "Verifying unified RX config"

    assert_config_y CONFIG_LASER_RX_UNIFIED
    assert_config_n CONFIG_LASER_SLE_JOB_RECEIVER
    assert_config_n CONFIG_LASER_SLE_JOB_TRANSMITTER

    local conflicting=(
        CONFIG_ENABLE_BT_SAMPLE
        CONFIG_ENABLE_PERIPHERAL_SAMPLE
        CONFIG_ENABLE_WIFI_SAMPLE
        CONFIG_ENABLE_PRODUCTS_SAMPLE
        CONFIG_ENABLE_RADAR_SAMPLE
        CONFIG_ENABLE_NFC_SAMPLE
        CONFIG_ENABLE_LASER_SINGLE_SAMPLE
        CONFIG_ENABLE_LASER_WIFI_SAMPLE
        CONFIG_ENABLE_LASER_SLE_JOB_SAMPLE
        CONFIG_ENABLE_SCREEN_SAMPLE
        CONFIG_ENABLE_LVGL_SAMPLE
        CONFIG_ENABLE_LVGL_PANEL
    )

    local symbol
    for symbol in "${conflicting[@]}"; do
        assert_config_n "$symbol"
    done

    assert_config_y CONFIG_LASER_RX_TRANSPORT_UART
    assert_config_y CONFIG_LASER_RX_TRANSPORT_WIFI
    assert_config_y CONFIG_LASER_RX_TRANSPORT_SLE_JOB
    assert_config_int CONFIG_LASER_RX_SLE_JOB_CACHE_SIZE 102400
    assert_config_int CONFIG_LASER_RX_WORK_AREA_X_MM 99
    assert_config_int CONFIG_LASER_RX_WORK_AREA_Y_MM 99
    assert_config_n CONFIG_LASER_RX_SLE_WAIT_TIMEOUT_MS

    local disabled_transports=(
        CONFIG_LASER_RX_UART_STATUS_PERIODIC
    )

    for symbol in "${disabled_transports[@]}"; do
        assert_config_n "$symbol"
    done

    echo "  Unified RX config OK: CONFIG_LASER_RX_UNIFIED=y"
    echo "  Standalone SLE role selectors OK: RECEIVER=not set, TRANSMITTER=not set"
    echo "  Routes compiled: SLE Job + Legacy WiFi + Legacy UART"
    echo "  Runtime policy: SLE Job primary; WiFi coexist disabled by default"
    echo "  SLE job cache fixed at 102400 bytes"
    echo "  Work area fixed at 99x99 mm"
}

resolve_unique_fwpkg() {
    if [[ ! -d "$FWPKG_DIR" ]]; then
        err "Firmware package directory not found: $FWPKG_DIR"
    fi

    local candidates=()
    mapfile -t candidates < <(find "$FWPKG_DIR" -maxdepth 1 -type f -name '*_all.fwpkg' | sort)

    if [[ "${#candidates[@]}" -eq 0 ]]; then
        err "No all-in-one firmware package found in ${FWPKG_DIR}"
    fi
    if [[ "${#candidates[@]}" -ne 1 ]]; then
        printf 'Found multiple all-in-one firmware package candidates:\n' >&2
        printf '  %s\n' "${candidates[@]}" >&2
        err "Expected exactly one *_all.fwpkg package in ${FWPKG_DIR}"
    fi
    if [[ "$(basename "${candidates[0]}")" != "$FWPKG_FILE" ]]; then
        err "Unexpected firmware package name: ${candidates[0]} (expected ${FWPKG_FILE})"
    fi

    echo "${candidates[0]}"
}

do_build() {
    log "Building unified RX firmware"

    local build_py
    build_py=$(find_build_py)
    local build_dir
    build_dir=$(dirname "$build_py")

    cd "$build_dir"
    echo "  Build command: $BUILD_CMD"
    echo "  Build directory: $build_dir"
    eval "$BUILD_CMD"

    verify_rx_unified_config

    local fwpkg
    fwpkg=$(resolve_unique_fwpkg)
    echo "  Build output: $fwpkg ($(du -h "$fwpkg" | cut -f1))"
}

archive() {
    local ts_dir="${STAGE_DIR}/${TIMESTAMP}"
    local latest_dir="${STAGE_DIR}/latest"
    local src
    src=$(resolve_unique_fwpkg)

    mkdir -p "$ts_dir" "$latest_dir"

    cp "$src" "${ts_dir}/${DEST_NAME}"
    cp "$src" "${latest_dir}/${DEST_NAME}"

    echo "  Archived: ${ts_dir}/${DEST_NAME}"
    echo "  Archived: ${latest_dir}/${DEST_NAME}"
}

generate_manifest() {
    local dir=$1
    local manifest="${dir}/manifest_rx_unified.txt"
    local fw="${dir}/${DEST_NAME}"
    local git_hash git_dirty git_status file_size file_sha

    git_hash=$(cd "$ROOT" && git rev-parse --short HEAD 2>/dev/null || echo "unknown")
    git_status=$(cd "$ROOT" && git status --porcelain 2>/dev/null | wc -l | tr -d ' ')
    if [[ "$git_status" -eq 0 ]]; then
        git_dirty="no"
    else
        git_dirty="yes (${git_status} modified files)"
    fi

    if [[ ! -f "$fw" ]]; then
        err "Cannot generate manifest; archived firmware not found: $fw"
    fi

    file_size=$(du -b "$fw" | cut -f1)
    file_sha=$(sha256sum "$fw" | cut -d' ' -f1)

    cat > "$manifest" <<EOF
firmware_type=rx_unified
phase=sle_job_primary_wifi_compiled_coexist_disabled
build_time=${TIMESTAMP}
git_commit=${git_hash}
git_dirty=${git_dirty}
build_command=${BUILD_CMD}
output_filename=${DEST_NAME}
output_path=${fw}
file_size=${file_size}
sha256=${file_sha}
enabled_sample_config=CONFIG_LASER_RX_UNIFIED=y
CONFIG_LASER_SLE_JOB_RECEIVER=not_set
CONFIG_LASER_SLE_JOB_TRANSMITTER=not_set
CONFIG_ENABLE_LASER_SINGLE_SAMPLE=not_set
CONFIG_ENABLE_LASER_WIFI_SAMPLE=not_set
CONFIG_ENABLE_LASER_SLE_JOB_SAMPLE=not_set
CONFIG_ENABLE_SCREEN_SAMPLE=not_set
CONFIG_ENABLE_LVGL_SAMPLE=not_set
CONFIG_LASER_RX_TRANSPORT_UART=y
CONFIG_LASER_RX_UART_BAUD=115200
CONFIG_LASER_RX_UART_STATUS_PERIODIC=not_set
CONFIG_LASER_RX_TRANSPORT_WIFI=y
CONFIG_LASER_RX_TRANSPORT_SLE_JOB=y
CONFIG_LASER_RX_SLE_JOB_CACHE_SIZE=102400
CONFIG_LASER_RX_WORK_AREA_X_MM=99
CONFIG_LASER_RX_WORK_AREA_Y_MM=99
CONFIG_LASER_RX_SLE_WAIT_TIMEOUT_MS=not_set
EOF

    echo "  Manifest: ${manifest}"
}

log "WS63 Unified RX Firmware Build"
echo "  Config:    ${CONFIG}"
echo "  Timestamp: ${TIMESTAMP}"
echo ""

switch_to_rx_unified
do_build
archive
generate_manifest "${STAGE_DIR}/${TIMESTAMP}"
generate_manifest "${STAGE_DIR}/latest"

echo ""
log "Build complete"
echo ""
echo "Unified RX firmware:"
echo "  ${STAGE_DIR}/latest/ws63-liteos-app_rx_unified_all.fwpkg"
echo ""
echo "Use BurnTool on Windows to flash this package manually."
