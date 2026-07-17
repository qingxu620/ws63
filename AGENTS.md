# fbb_ws63 Project Rules

## Project Scope

This WSL environment is dedicated to the fbb_ws63 project.

Project root:

```text
/root/fbb_ws63
```

Do not treat Windows paths under `/mnt/c` as the active development workspace unless explicitly instructed. Firmware source edits and builds happen in WSL2. Win11 is used for Host runtime, serial debugging, and BurnTool flashing.

## Current Architecture

Current product topology:

```text
Win11 Host UI
  -> TX command UART1
  -> TX board, src/ws63_laser_sle_tx
  -> SLE job packets
  -> RX board, src/ws63_laser_rx_unified
  -> motion / DAC / laser output

Screen board, src/ws63_screen_panel_lvgl_refactor
  -> local panel UI
  -> low-frequency RX status display / offline panel work
```

RX is the source of truth for motion execution, job state, cache state, and laser state. TX is a bridge from Host UART commands to structured SLE packets. The screen must not become a second independent motion controller unless the user explicitly requests a new architecture.

## Competition Board Identity Rules

This project is used in a competition environment. The active TX, RX, and Screen boards are the only valid peers. Do not add permissive discovery that can bind to unrelated development boards.

Current fixed board identities:

```text
RX board     SLE MAC: 20:06:09:27:12:01
Screen board SLE MAC: 20:06:09:27:12:02
TX board     SLE MAC: 20:06:09:27:12:03
```

Rules:

- TX must only connect to the fixed RX and fixed Screen board addresses.
- Screen Host Online Mode must only accept status mirror data from the fixed TX board address.
- Screen Panel Offline Mode must only connect to the fixed RX board address.
- Do not use loose device-name matching, service-UUID-only matching, magic-payload-only matching, or "first matching advertiser" behavior for product links.
- Unknown `adv_report` devices are noise. They may be logged for diagnostics, but must not update UI state, become job/control owners, or be used for SLE connections.
- When debugging Screen status discovery, keep the whitelist first, then parse status payloads from whitelisted peers only.

## Active Development Directories

Active directories:

- `src/ws63_laser_sle_tx/`: TX board firmware, UART-to-SLE job bridge.
- `src/ws63_laser_rx_unified/`: unified RX firmware, route-based receiver/executor.
- `src/ws63_laser_host_ui/`: Win11 Python Host UI.
- `src/ws63_screen_panel_lvgl_refactor/`: active MSP3223/ILI9341/FT6336 screen panel firmware.
- `MSP3223/`: active screen hardware reference package.
- `scripts/`: build/sync automation.

Do not modify SDK/vendor code, unrelated samples, or deprecated directories unless the user explicitly includes them in scope.

## Deprecated / Archive Directories

Deprecated directories are historical references, not active firmware lines:

- `src/deprecated/ws63_laser_sle_job/`: old standalone SLE job TX/RX experiment. Do not call this active RX. Do not build it as the product receiver.
- `src/deprecated/ws63_sd_card_test/`: standalone SD-card diagnostic firmware.
- `src/deprecated/ws63_laser_single/`: legacy UART laser route reference.
- `src/deprecated/ws63_laser_wifi/`: legacy WiFi laser route reference.
- `src/deprecated/ws63_screen_panel_lvgl/`: old LVGL minimal port.
- `src/deprecated/ws63_screen_st7796_ft6336/`: old screen self-test.

The product RX firmware name is always unified RX:

```text
ws63-liteos-app_rx_unified_all.fwpkg
```

Never document or recommend `ws63-liteos-app_rx_all.fwpkg` as the current RX firmware.

## Development Workflow

Before editing:

1. Read the relevant files.
2. Explain the current responsibility or data flow.
3. Identify the minimum files that need modification.
4. Explain the risk.
5. Make small, reviewable patches.

Do not rewrite whole source files unless strictly necessary. Do not make architecture changes without explicit confirmation.

## Git Rules

Before any commit:

1. Run `git status`.
2. Run `git diff --stat`.
3. Review the actual diff.
4. Summarize modified files by purpose.
5. Propose a commit message.

Commit only when explicitly requested. Never commit build outputs, temporary files, logs, or generated binaries.

Allowed commit message prefixes:

- `feat:`
- `fix:`
- `refactor:`
- `docs:`
- `chore:`
- `checkpoint:`

## Firmware Variants

The SDK raw package path is shared and overwritten on every build:

```text
src/output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg
```

Always use scripts that build one variant, then immediately archive it to:

```text
src/output/ws63/fwstage/latest/
```

Current firmware archives:

```text
src/output/ws63/fwstage/latest/ws63-liteos-app_tx_all.fwpkg
src/output/ws63/fwstage/latest/ws63-liteos-app_rx_unified_all.fwpkg
src/output/ws63/fwstage/latest/ws63-liteos-app_screen_all.fwpkg
src/output/ws63/fwstage/latest/ws63-liteos-app_sd_test_all.fwpkg
```

After any build script runs, `ws63_liteos_app.config` is left on that target's config. This is expected. Build the next target with its own script instead of assuming config state.

## TX Firmware Rules

Active TX firmware lives in:

```text
src/ws63_laser_sle_tx/
```

The SDK build switch is still named `CONFIG_ENABLE_LASER_SLE_JOB_SAMPLE=y` for compatibility, but in the current project it means active TX only. TX role selection is:

```text
CONFIG_LASER_SLE_JOB_TRANSMITTER=y
CONFIG_LASER_SLE_JOB_RECEIVER is not set
```

TX responsibilities:

- Accept Host commands and G-code bytes over UART1.
- Convert Host commands into framed SLE packets.
- Keep packet framing compatible with RX.
- Forward focus, stop, resume, abort, route switch, begin/data/end/status commands.
- Never directly control the laser output.

Current implementation snapshot from `src/ws63_laser_sle_tx/common/config.h`:

- UART command baud: `115200`
- SLE data chunk: `300B`
- SLE MTU: `512`
- RX connection interval selector: `0x10` (`JOB_SLE_CONN_INTERVAL_20MS`) in current config
- Panel connection interval selector: `0x20`
- Retry max: `3`
- Normal control ACK timeout: `2000 ms`; `EXEC_START` ACK timeout: `15000 ms`
- DATA transfer uses a small TX-side cumulative-offset window (`JOB_TX_DATA_WINDOW_PACKETS`, currently 3 packets) with hybrid SLE writes: `write_cmd` for DATA by default and `write_req` when forced for backpressure/confirmation. Host `@ACK type=2 offset=...` means TX accepted the chunk into the SLE DATA window; preroll and `JOB_END` must still drain until RX cumulative `received_size` reaches the boundary.
- DATA window progress timeout: `3000 ms`; when the window stalls, TX probes RX status and uses `STATUS_RESP.received_size` as cumulative confirmation.
- `JOB_END` is a commit/finalization confirmation, not the only way RX learns EOF. RX may auto-finalize when cumulative DATA reaches total size and CRC is valid; TX must treat delayed `JOB_END` ACK with status-probe/idempotent retry instead of immediately aborting a long cut job.

The current `300B` / window-3 / `0x10` / 4M-PHY-MCS10 direction is more aggressive than the older stable baseline. Do not describe it as hardware-stable until TX/RX board logs verify it. Treat 460800/921600 baud and any further payload, window, PHY, or interval increase as experiments unless the user explicitly asks for them again.

## Unified RX Firmware Rules

Active RX firmware lives in:

```text
src/ws63_laser_rx_unified/
```

Build switch:

```text
CONFIG_LASER_RX_UNIFIED=y
```

Current route model:

- SLE Job route is the active product route for Host/TX jobs.
- Legacy UART and WiFi code may compile for coverage or future routing, but they must not become independent concurrent motion controllers.
- Do not force UART/WiFi Grbl streaming and SLE job cache execution through one shared parser unless explicitly requested.
- Determine runtime state from `mode` and `active_route`, not from compiled route flags.

SLE Job route requirements:

- Cache size remains `102400`.
- Host preroll baseline remains `4096`.
- Treat the `4096` Host preroll baseline as the user-controlled upload-and-execute streaming policy. Do not bypass or disable upload-and-execute preroll just because a file fits in RX cache.
- Treat the `102400` RX cache size as the local SRAM/cache limit for receive storage, especially the upload-only path. Do not use it as a Host-side threshold to switch upload-and-execute jobs onto a different route.
- Preserve stable packet framing, CRC, ACK/NACK, sequence, duplicate handling, cache semantics, abort semantics, and reconnect behavior.
- Control packets must remain distinct from G-code data.
- Current stop controls are software safe-stop controls, not hardware emergency stop.

Mode switching requirements:

- Route switching commands must be explicit.
- Do not use single-character route changes or embed route control in G-code data.
- Any route switch must require IDLE, laser OFF, empty motion queue, no active SLE receive/execute job, and no GRBL stream in progress.
- A failed switch must force laser OFF and must not incorrectly update the active mode.

When modifying unified RX code or build integration, compile with:

```bash
./scripts/build_rx_unified_firmware.sh
```

## Laser Safety Rules

For laser-related code:

1. Any disconnect, abort, fatal parser error, route switch failure, or job cancellation must force laser off.
2. `M5` must physically disable PWM/GPIO output.
3. Do not rely only on modal/software state to indicate laser state.
4. RX must remain the source of truth for laser state.
5. Product-grade emergency shutdown should be implemented with hardware laser power/enable cut. Do not describe current SLE stop buttons as hard emergency stop.

## SLE / Transport Rules

When modifying SLE transport logic:

1. Check both TX and RX paths.
2. Preserve packet framing compatibility unless the protocol is explicitly changed.
3. Consider duplicate packets, lost ACKs, NACKs, credit/buffer overflow, timeout, reconnect, and stale command bytes.
4. Keep control packets distinguishable from `JOB_DATA`.
5. Do not let screen status listening interfere with Host/TX job transfer.
6. Prefer measured logs and small experiments over broad transport rewrites.

Known stable direction:

- Small stop-and-wait packets are slower but stable.
- Large payload / high baud / aggressive connection intervals have previously reduced stability.
- If throughput is revisited, isolate one variable at a time and keep a rollback point.

## WS63 / LiteOS Task Priority Rules

WS63 runs Huawei LiteOS-M. Priority numbering is inverted compared to FreeRTOS:

```text
Lower number = higher priority
0 = highest
31 = lowest
```

Current project convention:

| Task | Priority |
| --- | --- |
| RX SLE stack / RX work queue | 2 |
| TX SLE init | 3 |
| TX UART RX | 3 |
| Job executor | 4 |
| Motion | 4 |

Consequences:

- RX SLE priority 2 and TX SLE priority 3 can preempt executor priority 4.
- `osal_yield()` in a lower-priority task does not starve higher-priority SLE work.
- Use existing LiteOS/OSAL event, semaphore, queue, or notification primitives for synchronization.
- Do not introduce FreeRTOS-only APIs unless the SDK explicitly provides compatibility.
- Never increase a numeric priority thinking it makes the task higher priority.

## Host UI Rules

Host source:

```text
src/ws63_laser_host_ui/
```

Rules:

- Keep Python code simple and inspectable.
- Keep serial protocol behavior explicit.
- Preserve the user's upload-and-execute preroll choice. Do not silently change a configured `4096` preroll upload-and-execute job into a full upload then execute path based on file size or RX cache capacity.
- Prefer clear logs over hidden automation.
- Do not add large dependencies unless necessary.
- Host runs on Win11, not WSL2.
- Fix thread lifecycle issues carefully; avoid leaving `QThread` running on app exit.

Host synchronization:

```bash
cd /root/fbb_ws63
./scripts/sync_host_to_win.sh
```

Win11 run directory:

```text
C:\Users\ZKX\OneDrive\Desktop\ws63_laser_host_ui
```

## Screen Panel Rules

Active screen panel source:

```text
src/ws63_screen_panel_lvgl_refactor/
```

Current screen hardware:

- Module: MSP3223 3.2-inch SPI touch display
- LCD controller: ILI9341V, 240x320 RGB565
- Touch controller: FT6336U, I2C address `0x38`
- Reference package: `MSP3223/`

Current planned WS63 screen pins:

| Function | GPIO |
| --- | --- |
| LCD_SCK | GPIO7 |
| LCD_MOSI | GPIO9 |
| LCD_MISO | GPIO11 |
| LCD_CS | GPIO8 |
| LCD_DC | GPIO10 |
| LCD_RST | GPIO0 |
| LCD_BL | GPIO5 |
| CTP_SCL | GPIO16 / I2C1_SCL |
| CTP_SDA | GPIO15 / I2C1_SDA |
| CTP_RST | GPIO12 |
| CTP_INT | GPIO13 |
| SD_CS | GPIO14 |

Screen responsibilities:

- Show local panel UI.
- Show RX status using low-frequency status listening/broadcast behavior.
- Keep online Host job control authority with Host/TX/RX unless explicitly changed.
- Offline sender mode is separate from Host/TX online mode and must be tested separately.

Screen implementation rules:

- Avoid full-framebuffer designs unless memory is explicitly budgeted. A 240x320 RGB565 framebuffer is 153600 bytes.
- Prefer line buffers, tile buffers, or partial refresh.
- Do not use `src/4.0inch_SPI/` as an active reference.
- Do not modify LVGL core unless fixing a build warning or SDK integration issue.
- When modifying active screen code, compile screen firmware unless the user explicitly says not to compile.

Build active panel firmware:

```bash
./scripts/build_screen_firmware.sh --panel
```

Archive:

```text
src/output/ws63/fwstage/latest/ws63-liteos-app_screen_all.fwpkg
```

## SD Card Rules

The standalone SD diagnostic project is deprecated:

```text
src/deprecated/ws63_sd_card_test/
```

Build only when explicitly testing SD hardware/FAT behavior:

```bash
./scripts/build_sd_card_test_firmware.sh
```

Archive:

```text
src/output/ws63/fwstage/latest/ws63-liteos-app_sd_test_all.fwpkg
```

Do not reintroduce SD-card protocol experiments into active TX/RX code. The known SD hardware issue was card/media dependent, and the current product design should not depend on SD hot-plug robustness unless that feature is explicitly rescoped.

## Build Scripts

TX + unified RX:

```bash
cd /root/fbb_ws63
./scripts/build_sle_job_firmwares.sh --both
./scripts/build_sle_job_firmwares.sh --tx-only
./scripts/build_rx_unified_firmware.sh
```

Screen:

```bash
./scripts/build_screen_firmware.sh --panel
./scripts/build_screen_firmware.sh --lvgl
./scripts/build_screen_firmware.sh --selftest
```

SD diagnostic:

```bash
./scripts/build_sd_card_test_firmware.sh
```

Use raw SDK build commands only for menuconfig or low-level SDK debugging:

```bash
cd /root/fbb_ws63/src
python3 build.py -c ws63-liteos-app menuconfig
python3 build.py -c ws63-liteos-app -ninja -j24
```

## Win11 / Serial / BurnTool Rules

Do not auto-flash boards. BurnTool is manual on Win11.

Flash mapping:

- `ws63-liteos-app_tx_all.fwpkg` -> TX board
- `ws63-liteos-app_rx_unified_all.fwpkg` -> RX board
- `ws63-liteos-app_screen_all.fwpkg` -> screen board
- `ws63-liteos-app_sd_test_all.fwpkg` -> only for SD diagnostic board/test

COM ports are not fixed. Do not assume historical COM numbers are still valid.

Before giving Win11 run/debug steps, ask for the current role mapping:

- TX command UART1 COM port
- TX log UART0 COM port
- RX log UART0 COM port
- Whether TX command UART1 is still 115200

Stable baud baseline:

- TX command UART1: `115200`
- TX log UART0: `115200`
- RX log UART0: `115200`

Do not assume WSL2 can access COM ports directly.

## Manual Focus

Protocol:

- `@FOCUS_ON S{0-100}` -> TX forwards `PKT_FOCUS_CTRL`
- `@FOCUS_OFF` -> TX forwards `PKT_FOCUS_CTRL`
- RX only allows focus on in IDLE.
- RX allows focus off unconditionally.

Safety:

- `EXEC_START`, `EXEC_STOP`, `ABORT`, disconnect, safe stop, route switch, and job done must force focus/laser off.
- TX does not directly control laser output.

Key files:

- `src/ws63_laser_sle_tx/common/protocol.h`
- `src/ws63_laser_sle_tx/transmitter/main.c`
- `src/ws63_laser_rx_unified/routes/sle_job/sle_job_manager.c`
- `src/ws63_laser_host_ui/main.py`

Changing `PKT_FOCUS_CTRL` or its payload requires rebuilding and flashing both TX and RX firmware.

## Demo Stable Checklist

Before a demo run:

1. Confirm current Win11 three-port serial mapping.
2. Keep all three involved UARTs at 115200 unless explicitly testing baud changes.
3. Verify `@STATUS`.
4. Verify focus on/off at low power.
5. Run a short G-code job.
6. Run a large file with `preroll=4096`.
7. Confirm `EXEC_DONE`.
8. Confirm physical laser off after completion, abort, disconnect, and failure.
9. If screen is present, verify status display does not destabilize Host/TX/RX transfer.

## Verified Audit Snapshot (2026-07-12)

This snapshot records the 2026-07-12 audit and the source fixes applied afterward. Do not call a firmware fix board-verified merely because Host tests or host-GCC syntax checks pass. Re-audit the cited paths after further changes and require the relevant product build plus board tests before closing hardware/SLE lifecycle work.

Build and verification blocker:

- TX, unified RX, and Screen panel builds all stop during common SDK CMake configuration because `src/middleware/utils/at/at_bt_cmd/CMakeLists.txt` requires missing `src/at_bt_cmd_register.c` and `at/at_bt_productline.c`, while the fallback `ws63-liteos-app/libbt_at.a` also does not exist in this checkout. This blocks formal compilation of active project sources; restore the matching SDK component instead of fabricating a stub or editing vendor code around it. Old staged firmware is not proof that the current tree builds.

Source fixes applied; build and/or board verification still required:

- Unified RX now admits only the fixed TX and fixed Screen MACs, while the first whitelisted writer remains the single control owner. Verify TX online and Screen offline ownership/reconnect behavior on three boards.
- Screen status discovery now selects the fixed TX in Online View and the fixed RX in Offline View before parsing/applying status.
- RX replay detection now uses 16-bit circular distance, so the reserved-zero wrap `65535 -> 1` remains valid while genuinely old packets are dropped.
- RX cache rejects a DATA range beyond declared `total_size` before copying or updating CRC.
- DAC writes/recovery now return failures; all compiled motion executors force laser off, disarm output, and abort the current move on a failed galvo write. RX initializes the laser-off path before DAC setup.
- Laser PWM init/open/update/group/start failures now keep the physical pin low and clear logical enabled/effective-power state.
- SLE route restart now keeps one configured server/service instance, stops accepting work and advertising during route stop, and resumes advertising without repeating `enable_sle()` or server registration. Verify SLE -> WiFi -> SLE on board.
- Screen cache telemetry now uses the product `102400`-byte cache size.
- Host validation mirrors RX G90/G91/G28/G92 position state and rejects out-of-area cumulative targets; regression tests cover negative in-range relative moves and cumulative overflow.
- `FOCUS_ON S0` is rejected by Host configuration/UI defenses, TX, Screen, and RX; the laser driver also refuses logical enable at zero power.

Robustness mitigation requiring timing validation:

- TX now answers queued duplicate UART CAN bytes from a recent successful idle resync without issuing another synchronous RX abort. Validate with delayed/lost `JOB_ABORT` ACK injection; failed aborts intentionally remain retryable.

## Response Style

When working in this repository, prefer this structure:

1. Current problem
2. Root cause
3. Minimal fix
4. Affected files
5. Verification method

For small tasks, keep the answer short and focus on what changed and how it was verified.
