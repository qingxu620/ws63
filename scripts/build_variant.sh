#!/usr/bin/env bash
#===============================================================================
# WS63 Firmware Variant Builder
#
# Unified entry point for building TX, unified RX, or Screen Panel firmware.
# Features:
#   - Flock-based mutual exclusion (prevents concurrent builds)
#   - Atomic config backup/restore with trap cleanup
#   - Pre/post-build variant verification
#   - Independent archived output per variant
#   - Global latest-firmware publication after a successful build
#   - build-info.json generation
#
# Usage:
#   scripts/build_variant.sh tx
#   scripts/build_variant.sh rx_unified
#   scripts/build_variant.sh screen_panel
#   scripts/build_variant.sh wenxuan
#   scripts/build_variant.sh --help
#===============================================================================
set -euo pipefail

ROOT="/root/fbb_ws63"
CONFIG="${ROOT}/src/build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config"
CONFIG_OLD="${CONFIG}.old"
FWPKG_DIR="${ROOT}/src/output/ws63/fwpkg/ws63-liteos-app"
FWPKG_FILE="ws63-liteos-app_all.fwpkg"
BUILD_DIR="${ROOT}/src"
BUILD_CMD="python3 build.py -c ws63-liteos-app -ninja -j24"
LOCK_FILE="/tmp/fbb_ws63_ws63_liteos_app_build.lock"
STAGE_DIR="${ROOT}/src/output/ws63/fwstage"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
GIT_HASH=$(cd "$ROOT" && git rev-parse --short HEAD 2>/dev/null || echo "unknown")
GIT_DIRTY=$(cd "$ROOT" && git status --porcelain 2>/dev/null | wc -l || echo "0")

BACKUP_DIR=""
ORIG_CONFIG_HASH=""
ORIG_CONFIG_OLD_HASH=""
BUILD_RC=-1

# ---- Cleanup trap -----------------------------------------------------------
cleanup() {
    local rc=$?
    set +euo pipefail

    # Release lock
    exec 9>&- 2>/dev/null || true

    # Restore original config
    if [ -n "$BACKUP_DIR" ] && [ -d "$BACKUP_DIR" ]; then
        if [ -f "${BACKUP_DIR}/ws63_liteos_app.config" ]; then
            cp "${BACKUP_DIR}/ws63_liteos_app.config" "$CONFIG"
        fi
        if [ -f "${BACKUP_DIR}/ws63_liteos_app.config.old" ]; then
            cp "${BACKUP_DIR}/ws63_liteos_app.config.old" "$CONFIG_OLD"
        elif [ -f "$CONFIG_OLD" ]; then
            # Backup had no .config.old - the build may have created it
            if [ -n "$ORIG_CONFIG_OLD_HASH" ]; then
                rm -f "$CONFIG_OLD" 2>/dev/null || true
            fi
        fi
        rm -rf "$BACKUP_DIR" 2>/dev/null || true
    fi

    # If we were interrupted, don't mask the original RC
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 130 ] && [ "$rc" -ne 143 ]; then
        exit "$rc"
    fi
}
trap cleanup EXIT INT TERM HUP

# ---- Lock -------------------------------------------------------------------
acquire_lock() {
    exec 9>"$LOCK_FILE"
    if ! flock -n 9; then
        echo "ERROR: Another ws63-liteos-app build is already running." >&2
        echo "  Lock file: ${LOCK_FILE}" >&2
        exit 1
    fi
}

# ---- Config functions -------------------------------------------------------
set_config_y() {
    local symbol=$1
    local tmp="$2"
    if grep -q "^${symbol}=y$" "$tmp"; then return; fi
    if grep -q "^# ${symbol} is not set$" "$tmp"; then
        sed -i "s/^# ${symbol} is not set$/${symbol}=y/" "$tmp"; return
    fi
    if grep -q "^${symbol}=" "$tmp"; then
        sed -i "s/^${symbol}=.*/${symbol}=y/" "$tmp"; return
    fi
    printf '%s=y\n' "$symbol" >> "$tmp"
}

set_config_n() {
    local symbol=$1
    local tmp="$2"
    if grep -q "^# ${symbol} is not set$" "$tmp"; then return; fi
    if grep -q "^${symbol}=y$" "$tmp"; then
        sed -i "s/^${symbol}=y$/# ${symbol} is not set/" "$tmp"; return
    fi
    if grep -q "^${symbol}=" "$tmp"; then
        sed -i "s/^${symbol}=.*/# ${symbol} is not set/" "$tmp"; return
    fi
    printf '# %s is not set\n' "$symbol" >> "$tmp"
}

set_config_int() {
    local symbol=$1
    local value=$2
    local tmp="$3"
    if grep -q "^${symbol}=" "$tmp"; then
        sed -i "s/^${symbol}=.*/${symbol}=${value}/" "$tmp"
    else
        printf '%s=%s\n' "$symbol" "$value" >> "$tmp"
    fi
}

# ---- Variant config switching -----------------------------------------------
switch_to_tx() {
    local tmp="$1"

    set_config_n CONFIG_ENABLE_LASER_SINGLE_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_LASER_WIFI_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_LASER_SLE_JOB_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_SCREEN_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_LVGL_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_LVGL_PANEL "$tmp"
    set_config_n CONFIG_LASER_RX_UNIFIED "$tmp"
    set_config_n CONFIG_LASER_RX_WENXUAN "$tmp"
    set_config_n CONFIG_LASER_RX_SLE_JOB_ALLOW_PHONE "$tmp"
    set_config_n CONFIG_ENABLE_SD_CARD_TEST "$tmp"

    set_config_y CONFIG_ENABLE_LASER_SLE_JOB_SAMPLE "$tmp"
    set_config_n CONFIG_LASER_SLE_JOB_RECEIVER "$tmp"
    set_config_y CONFIG_LASER_SLE_JOB_TRANSMITTER "$tmp"
    set_config_int CONFIG_UART1_BAUDRATE 115200 "$tmp"
    set_config_int CONFIG_LOG_UART_BAUDRATE 115200 "$tmp"
    set_config_int CONFIG_LASER_SLE_JOB_UART_BAUD 115200 "$tmp"
}

switch_to_rx_unified() {
    local tmp="$1"

    set_config_y CONFIG_SAMPLE_ENABLE "$tmp"
    set_config_n CONFIG_ENABLE_LASER_SINGLE_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_LASER_WIFI_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_LASER_SLE_JOB_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_SCREEN_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_LVGL_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_LVGL_PANEL "$tmp"
    set_config_n CONFIG_ENABLE_SD_CARD_TEST "$tmp"
    set_config_n CONFIG_LASER_SLE_JOB_RECEIVER "$tmp"
    set_config_n CONFIG_LASER_SLE_JOB_TRANSMITTER "$tmp"

    set_config_y CONFIG_LASER_RX_UNIFIED "$tmp"
    set_config_n CONFIG_LASER_RX_WENXUAN "$tmp"
    set_config_y CONFIG_LASER_RX_TRANSPORT_UART "$tmp"
    set_config_y CONFIG_LASER_RX_TRANSPORT_WIFI "$tmp"
    set_config_y CONFIG_LASER_RX_TRANSPORT_SLE_JOB "$tmp"
    set_config_y CONFIG_LASER_RX_SLE_JOB_ALLOW_PHONE "$tmp"
    set_config_int CONFIG_LASER_RX_SLE_JOB_CACHE_SIZE 102400 "$tmp"
    set_config_int CONFIG_LASER_RX_UART_BAUD 115200 "$tmp"
    set_config_int CONFIG_LASER_RX_WORK_AREA_X_MM 99 "$tmp"
    set_config_int CONFIG_LASER_RX_WORK_AREA_Y_MM 99 "$tmp"
}

switch_to_screen_panel() {
    local tmp="$1"

    set_config_n CONFIG_ENABLE_LASER_SINGLE_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_LASER_WIFI_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_LASER_SLE_JOB_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_SCREEN_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_LVGL_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_LVGL_PANEL "$tmp"
    set_config_n CONFIG_ENABLE_SD_CARD_TEST "$tmp"
    set_config_n CONFIG_LASER_RX_UNIFIED "$tmp"
    set_config_n CONFIG_LASER_RX_WENXUAN "$tmp"
    set_config_n CONFIG_LASER_RX_SLE_JOB_ALLOW_PHONE "$tmp"
    set_config_n CONFIG_LASER_SLE_JOB_RECEIVER "$tmp"
    set_config_n CONFIG_LASER_SLE_JOB_TRANSMITTER "$tmp"

    set_config_y CONFIG_ENABLE_LVGL_PANEL "$tmp"
    set_config_y CONFIG_SPI_SUPPORT_DMA "$tmp"
    set_config_n CONFIG_SPI_SUPPORT_POLL_AND_DMA_AUTO_SWITCH "$tmp"
}

switch_to_wenxuan() {
    local tmp="$1"

    set_config_y CONFIG_SAMPLE_ENABLE "$tmp"
    set_config_n CONFIG_ENABLE_LASER_SINGLE_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_LASER_WIFI_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_LASER_SLE_JOB_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_SCREEN_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_LVGL_SAMPLE "$tmp"
    set_config_n CONFIG_ENABLE_LVGL_PANEL "$tmp"
    set_config_n CONFIG_ENABLE_SD_CARD_TEST "$tmp"
    set_config_n CONFIG_LASER_RX_UNIFIED "$tmp"
    set_config_n CONFIG_LASER_SLE_JOB_RECEIVER "$tmp"
    set_config_n CONFIG_LASER_SLE_JOB_TRANSMITTER "$tmp"

    set_config_y CONFIG_LASER_RX_WENXUAN "$tmp"
    set_config_y CONFIG_LASER_RX_TRANSPORT_UART "$tmp"
    set_config_y CONFIG_LASER_RX_TRANSPORT_WIFI "$tmp"
    set_config_y CONFIG_LASER_RX_TRANSPORT_SLE_JOB "$tmp"
    set_config_y CONFIG_LASER_RX_SLE_JOB_ALLOW_PHONE "$tmp"
    set_config_int CONFIG_LASER_RX_SLE_JOB_CACHE_SIZE 102400 "$tmp"
    set_config_int CONFIG_LASER_RX_UART_BAUD 115200 "$tmp"
    set_config_int CONFIG_LASER_RX_WORK_AREA_X_MM 99 "$tmp"
    set_config_int CONFIG_LASER_RX_WORK_AREA_Y_MM 99 "$tmp"
}

apply_config() {
    local variant="$1"
    local backup_config="${BACKUP_DIR}/ws63_liteos_app.config"
    local tmp_config

    # Copy original config to backup
    cp "$CONFIG" "$backup_config"
    if [ -f "$CONFIG_OLD" ]; then
        cp "$CONFIG_OLD" "${BACKUP_DIR}/ws63_liteos_app.config.old"
    fi

    # Create temp config
    tmp_config=$(mktemp "${CONFIG}.tmp.XXXXXX")
    cp "$CONFIG" "$tmp_config"

    echo "  Applying config: ${variant}"
    case "$variant" in
        tx) switch_to_tx "$tmp_config" ;;
        rx_unified) switch_to_rx_unified "$tmp_config" ;;
        screen_panel) switch_to_screen_panel "$tmp_config" ;;
        wenxuan) switch_to_wenxuan "$tmp_config" ;;
        *) echo "ERROR: Unknown variant: ${variant}" >&2; exit 1 ;;
    esac

    # Atomic replace
    mv "$tmp_config" "$CONFIG"
}

# ---- Variant verification ---------------------------------------------------
check_variant_selected() {
    local variant="$1"

    local tx_on=0 rx_on=0 screen_on=0 wenxuan_on=0 phone_on=0
    grep -q '^CONFIG_LASER_SLE_JOB_TRANSMITTER=y$' "$CONFIG" && tx_on=1
    grep -q '^CONFIG_LASER_RX_UNIFIED=y$' "$CONFIG" && rx_on=1
    grep -q '^CONFIG_ENABLE_LVGL_PANEL=y$' "$CONFIG" && screen_on=1
    grep -q '^CONFIG_LASER_RX_WENXUAN=y$' "$CONFIG" && wenxuan_on=1
    grep -q '^CONFIG_LASER_RX_SLE_JOB_ALLOW_PHONE=y$' "$CONFIG" && phone_on=1

    local total=$((tx_on + rx_on + screen_on + wenxuan_on))
    if [ "$total" -ne 1 ]; then
        echo "ERROR: Exactly one firmware variant must be selected, got ${total}" >&2
        echo "  TX: ${tx_on}, RX: ${rx_on}, Screen: ${screen_on}, Wenxuan: ${wenxuan_on}" >&2
        exit 1
    fi

    case "$variant" in
        tx)
            if [ "$tx_on" -ne 1 ]; then echo "ERROR: TX config not applied" >&2; exit 1; fi
            if [ "$phone_on" -ne 0 ]; then echo "ERROR: TX must not enable phone admission" >&2; exit 1; fi
            ;;
        rx_unified)
            if [ "$rx_on" -ne 1 ]; then echo "ERROR: RX config not applied" >&2; exit 1; fi
            if [ "$phone_on" -ne 1 ]; then echo "ERROR: Unified RX phone admission not applied" >&2; exit 1; fi
            ;;
        screen_panel)
            if [ "$screen_on" -ne 1 ]; then echo "ERROR: Screen config not applied" >&2; exit 1; fi
            if [ "$phone_on" -ne 0 ]; then echo "ERROR: Screen must not enable phone admission" >&2; exit 1; fi
            ;;
        wenxuan)
            if [ "$wenxuan_on" -ne 1 ]; then echo "ERROR: Wenxuan config not applied" >&2; exit 1; fi
            if [ "$phone_on" -ne 1 ]; then echo "ERROR: Wenxuan phone admission not applied" >&2; exit 1; fi
            ;;
    esac
    echo "  Verified: ${variant} selected"
}

# ---- Build ------------------------------------------------------------------
do_build() {
    local variant="$1"
    local log_file="$2"

    echo "  Build command: ${BUILD_CMD}"
    echo "  Log file: ${log_file}"

    cd "$BUILD_DIR"
    set +e
    eval "$BUILD_CMD" 2>&1 | tee "$log_file"
    BUILD_RC=${PIPESTATUS[0]}
    set -e

    if [ "$BUILD_RC" -ne 0 ]; then
        echo "ERROR: Build failed with exit code ${BUILD_RC}" >&2
        exit "$BUILD_RC"
    fi

    # Verify build output
    local fwpkg="${FWPKG_DIR}/${FWPKG_FILE}"
    if [ ! -f "$fwpkg" ]; then
        echo "ERROR: Firmware package not found: ${fwpkg}" >&2
        exit 1
    fi
    if [ ! -s "$fwpkg" ]; then
        echo "ERROR: Firmware package is empty: ${fwpkg}" >&2
        exit 1
    fi

    echo "  Build output: ${fwpkg} ($(du -h "$fwpkg" | cut -f1))"
}

# ---- Archive ----------------------------------------------------------------
publish_global_firmware() {
    local source_file="$1"
    local global_dir="$2"
    local output_name="$3"
    local temporary_file

    temporary_file=$(mktemp "${global_dir}/.${output_name}.tmp.XXXXXX")
    if ! cp "$source_file" "$temporary_file"; then
        rm -f "$temporary_file"
        return 1
    fi
    mv -f "$temporary_file" "${global_dir}/${output_name}"
}

archive() {
    local variant="$1"
    local log_file="$2"

    local variant_dir="${STAGE_DIR}/${variant}"
    local run_dir="${variant_dir}/${TIMESTAMP}-${GIT_HASH}"
    local latest_link="${variant_dir}/latest"
    local global_latest_dir="${STAGE_DIR}/latest"

    mkdir -p "$run_dir" "$global_latest_dir"

    local src_fwpkg="${FWPKG_DIR}/${FWPKG_FILE}"
    local src_elf="${BUILD_DIR}/output/ws63/acore/ws63-liteos-app/ws63-liteos-app.elf"
    local src_bin="${BUILD_DIR}/output/ws63/acore/ws63-liteos-app/ws63-liteos-app.bin"
    local src_sign="${BUILD_DIR}/output/ws63/acore/ws63-liteos-app/ws63-liteos-app-sign.bin"
    local src_map="${BUILD_DIR}/output/ws63/acore/ws63-liteos-app/ws63-liteos-app.map"
    local src_mconfig="${BUILD_DIR}/output/ws63/acore/ws63-liteos-app/mconfig.h"

    local dest_name global_dest_name obsolete_global_name=""
    case "$variant" in
        tx)
            dest_name="ws63-liteos-app_tx"
            global_dest_name="ws63-liteos-app_tx_all.fwpkg"
            ;;
        rx_unified)
            dest_name="ws63-liteos-app_rx_unified"
            global_dest_name="ws63-liteos-app_rx_unified_all.fwpkg"
            ;;
        screen_panel)
            dest_name="ws63-liteos-app_screen_panel"
            global_dest_name="ws63-liteos-app_screen_all.fwpkg"
            obsolete_global_name="ws63-liteos-app_screen_panel_all.fwpkg"
            ;;
        wenxuan)
            dest_name="ws63-liteos-app_wenxuan"
            global_dest_name="ws63-liteos-app_wenxuan_all.fwpkg"
            ;;
    esac

    # Copy artifacts
    cp "$src_fwpkg" "${run_dir}/${dest_name}_all.fwpkg"
    [ -f "$src_elf" ] && cp "$src_elf" "${run_dir}/${dest_name}.elf"
    [ -f "$src_bin" ] && cp "$src_bin" "${run_dir}/${dest_name}.bin"
    [ -f "$src_sign" ] && cp "$src_sign" "${run_dir}/${dest_name}-sign.bin"
    [ -f "$src_map" ] && cp "$src_map" "${run_dir}/${dest_name}.map"
    if [ -f "$log_file" ] && [ "$log_file" != "${run_dir}/build.log" ]; then
        cp "$log_file" "${run_dir}/build.log"
    fi
    cp "$CONFIG" "${run_dir}/effective.config"
    [ -f "$src_mconfig" ] && cp "$src_mconfig" "${run_dir}/mconfig.h"

    # Generate SHA-256
    cd "$run_dir"
    sha256sum *.fwpkg *.elf *.bin 2>/dev/null > sha256sums.txt || true
    cd "$ROOT"

    # Generate build-info.json
    local fw_sha256=""
    local elf_size=0 bin_size=0 fwpkg_size=0
    fwpkg_size=$(stat --format=%s "${run_dir}/${dest_name}_all.fwpkg" 2>/dev/null || echo 0)
    if [ -f "${run_dir}/${dest_name}.elf" ]; then
        elf_size=$(stat --format=%s "${run_dir}/${dest_name}.elf" 2>/dev/null || echo 0)
        fw_sha256=$(sha256sum "${run_dir}/${dest_name}.elf" 2>/dev/null | cut -d' ' -f1 || echo "")
    fi
    if [ -f "${run_dir}/${dest_name}.bin" ]; then
        bin_size=$(stat --format=%s "${run_dir}/${dest_name}.bin" 2>/dev/null || echo 0)
    fi

    cat > "${run_dir}/build-info.json" <<EOF
{
  "variant": "${variant}",
  "timestamp": "${TIMESTAMP}",
  "project_commit": "${GIT_HASH}",
  "working_tree_dirty": $([ "$GIT_DIRTY" -gt 0 ] && echo "true" || echo "false"),
  "sdk_version": "1.10.106",
  "sdk_tag": "1.10.106",
  "sdk_commit": "7cda10e7476d8df35f0ac1a4013eb395874247e0",
  "toolchain": "riscv32-linux-musl-gcc 7.3.0 (build ver105.010)",
  "build_command": "${BUILD_CMD}",
  "build_exit_code": ${BUILD_RC},
  "artifacts": {
    "fwpkg": "${dest_name}_all.fwpkg",
    "fwpkg_size": ${fwpkg_size},
    "elf": "${dest_name}.elf",
    "elf_size": ${elf_size},
    "bin": "${dest_name}.bin",
    "bin_size": ${bin_size},
    "elf_sha256": "${fw_sha256}"
  },
  "firmware_sha256": "$(sha256sum "${run_dir}/${dest_name}_all.fwpkg" 2>/dev/null | cut -d' ' -f1 || echo "")"
}
EOF

    # Update latest symlink
    rm -f "$latest_link"
    ln -sf "$run_dir" "$latest_link"

    # Publish the packaged firmware to the project-wide latest directory only
    # after the per-variant archive is complete. Each replacement is atomic so
    # a reader never sees a partially written firmware package.
    publish_global_firmware "${run_dir}/${dest_name}_all.fwpkg" "$global_latest_dir" "$global_dest_name"
    if [ -n "$obsolete_global_name" ]; then
        # Screen has one product firmware name in the project-wide latest
        # directory. Remove the former variant-name alias only after the
        # canonical package has been published successfully.
        rm -f "${global_latest_dir}/${obsolete_global_name}"
    fi

    echo "  Archives: ${run_dir}/"
    echo "  Latest:   ${latest_link}/"
    echo "  Global latest: ${global_latest_dir}/${global_dest_name}"
}

# ---- Help -------------------------------------------------------------------
usage() {
    cat <<EOF
Usage: $0 <variant>

Variants:
  tx             Build SLE job transmitter firmware
  rx_unified     Build phone-enabled unified laser receiver firmware
  screen_panel   Build LVGL screen panel firmware
  wenxuan        Build phone-enabled Wenxuan receiver firmware

Options:
  --help         Show this help

The script automatically:
  - Acquires a build lock (prevents concurrent builds)
  - Backs up and restores the shared .config
  - Applies variant-specific configuration atomically
  - Verifies exactly one variant is selected before/after build
  - Archives artifacts to fwstage/<variant>/<timestamp>-<commit>/
  - Publishes the packaged firmware to fwstage/latest/
  - Restores original .config on exit
EOF
    exit 0
}

# ---- Main -------------------------------------------------------------------
main() {
    local variant="${1:-}"
    case "$variant" in
        ""|--help|-h) usage ;;
        tx|rx_unified|screen_panel|wenxuan) ;;
        *) echo "ERROR: Unknown variant: ${variant}"; usage ;;
    esac

    echo "=== WS63 Firmware Build: ${variant} ==="
    echo "  Timestamp: ${TIMESTAMP}"
    echo "  Commit:    ${GIT_HASH}"
    echo ""

    acquire_lock
    echo "  Lock acquired"

    # Back up config
    BACKUP_DIR=$(mktemp -d)
    echo "  Config backup: ${BACKUP_DIR}"

    # Record original hashes
    if [ -f "$CONFIG" ]; then
        ORIG_CONFIG_HASH=$(sha256sum "$CONFIG" | cut -d' ' -f1)
    fi
    if [ -f "$CONFIG_OLD" ]; then
        ORIG_CONFIG_OLD_HASH=$(sha256sum "$CONFIG_OLD" | cut -d' ' -f1)
    fi

    # Apply variant config atomically
    apply_config "$variant"

    # Verify before build
    check_variant_selected "$variant"

    # Build
    local log_file="${STAGE_DIR}/${variant}/${TIMESTAMP}-${GIT_HASH}/build.log"
    mkdir -p "$(dirname "$log_file")"
    do_build "$variant" "$log_file"

    # Post-build verification (from mconfig.h)
    local mconfig="${BUILD_DIR}/output/ws63/acore/ws63-liteos-app/mconfig.h"
    if [ -f "$mconfig" ]; then
        echo "  Post-build config check:"
        local tx_in=0 rx_in=0 screen_in=0 wenxuan_in=0 phone_in=0
        grep -q 'CONFIG_LASER_SLE_JOB_TRANSMITTER 1' "$mconfig" && tx_in=1
        grep -q 'CONFIG_LASER_RX_UNIFIED 1' "$mconfig" && rx_in=1
        grep -q 'CONFIG_ENABLE_LVGL_PANEL 1' "$mconfig" && screen_in=1
        grep -q 'CONFIG_LASER_RX_WENXUAN 1' "$mconfig" && wenxuan_in=1
        grep -q 'CONFIG_LASER_RX_SLE_JOB_ALLOW_PHONE 1' "$mconfig" && phone_in=1
        if [ "$((tx_in + rx_in + screen_in + wenxuan_in))" -ne 1 ]; then
            echo "  WARNING: mconfig.h shows ${tx_in} TX + ${rx_in} RX + ${screen_in} Screen + ${wenxuan_in} Wenxuan" >&2
        else
            echo "  OK: mconfig.h matches ${variant}"
        fi
        case "$variant" in
            rx_unified|wenxuan)
                if [ "$phone_in" -ne 1 ]; then
                    echo "ERROR: mconfig.h is missing phone admission for ${variant}" >&2
                    exit 1
                fi
                ;;
            tx|screen_panel)
                if [ "$phone_in" -ne 0 ]; then
                    echo "ERROR: mconfig.h unexpectedly enables phone admission for ${variant}" >&2
                    exit 1
                fi
                ;;
        esac
    fi

    # Archive
    archive "$variant" "$log_file"

    echo ""
    echo "=== Build complete: ${variant} (exit code ${BUILD_RC}) ==="
}

main "$@"
