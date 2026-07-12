# Building WS63 Firmware

## SDK Baseline

| Item | Value |
|---|---|
| Repository | https://gitee.com/HiSpark/fbb_ws63.git |
| SDK version / tag | 1.10.106 |
| Official commit | 7cda10e7476d8df35f0ac1a4013eb395874247e0 |
| License | Apache 2.0 (see `LICENSE`) |

The SDK source is forked from the HiSpark repository. There is no shared
git history with the upstream; fixes must be cherry-picked manually.

## LVGL Baseline

| Item | Value |
|---|---|
| Repository | https://github.com/lvgl/lvgl |
| Tag | v9.3.0 |
| Commit | c033a98afddd65aaafeebea625382a94020fe4a7 |
| License | MIT |
| Location | `src/deprecated/ws63_screen_lvgl/src/lvgl/` |

Only the `src/core/` subdirectory and `LICENCE.txt` were imported from
the upstream tag to supplement the existing LVGL tree.

## Prerequisites

- Python 3.10+
- CMake 3.22+
- Ninja 1.10+
- RISC-V toolchain (included at `src/tools/bin/compiler/riscv/`)

## Dependency Verification

Before building, verify that all required SDK precompiled libraries and
LVGL source files are present:

```bash
scripts/verify_dependencies.sh
```

Expected output: "75 passed, 0 failed, 0 skipped"

## Restoring SDK Libraries

If the 22 precompiled `.a` libraries are missing (e.g., after a fresh clone
when they are not yet tracked), restore them from the official SDK:

```bash
scripts/restore_official_sdk_libs.sh --restore
```

To verify without downloading:

```bash
scripts/restore_official_sdk_libs.sh --verify-only
```

The restore script only fetches from the locked HiSpark SDK tag
(`1.10.106`, commit `7cda10e7`) and verifies SHA-256 checksums.

## Building Firmware

Three firmware variants are available:

```bash
# SLE job transmitter (TX board)
scripts/build_variant.sh tx

# Unified laser receiver (RX board)
scripts/build_variant.sh rx_unified

# LVGL screen panel (screen board)
scripts/build_variant.sh screen_panel
```

Build all three sequentially:

```bash
scripts/build_all_variants.sh
```

### Build Behavior

- **Concurrent builds are prevented** by a `flock` lock file.
- The shared `.config` file is **backed up before the build** and **restored
  on exit** (including on Ctrl+C or failure).
- Exactly **one firmware variant must be selected**; the CMake configure
  step enforces this and will `FATAL_ERROR` if zero or multiple variants
  are active.
- Builds cannot run in parallel because all variants share the same SDK
  build directory (`output/ws63/acore/ws63-liteos-app/`).

### Output

Artifacts are archived per variant:

```
src/output/ws63/fwstage/
  tx/latest/
  rx_unified/latest/
  screen_panel/latest/
```

Each archive directory contains:

| File | Description |
|---|---|
| `*.fwpkg` | Flashable firmware package (ELF + BIN + metadata) |
| `*.elf` | ELF executable |
| `*.bin` | Raw binary |
| `*-sign.bin` | Signed binary |
| `*.map` | Linker map |
| `build.log` | Full build log |
| `effective.config` | Kconfig configuration used for this build |
| `mconfig.h` | Auto-generated config header |
| `sha256sums.txt` | SHA-256 checksums of all binaries |
| `build-info.json` | Build metadata (commit, toolchain, variant) |

### Important Notes

- **Build success does not equal hardware verification.** SLE connection,
  job transfer, emergency stop, and laser safety must be validated on
  actual boards.
- **Precompiled libraries cannot be source-audited.** The 22 SDK `.a`
  files are from HiSilicon's SDK and are provided as-is under Apache 2.0.
- The three legacy scripts (`build_sle_job_firmwares.sh`,
  `build_rx_unified_firmware.sh`, `build_screen_firmware.sh`) still work
  but delegate to `build_variant.sh`. Set `USE_LEGACY_BUILD=true` to use
  the original logic.

## Defconfig Templates

Pre-built configuration templates are available at:

```
configs/ws63_tx_defconfig
configs/ws63_rx_unified_defconfig
configs/ws63_screen_panel_defconfig
```

These can be copied to the active `.config` path:

```bash
cp configs/ws63_rx_unified_defconfig \
  src/build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config
```
