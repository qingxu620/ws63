# fbb_ws63 OpenCode Project Rules

## Project Scope

This WSL environment is dedicated to the fbb_ws63 project only.

The project root is:

```text
/root/fbb_ws63
```

Do not treat Windows paths under /mnt/c as the active development workspace unless explicitly instructed.

## Active Development Directories

The current active development focus is:

- src/ws63_laser_sle_job/
- src/ws63_laser_host_ui/
- src/ws63_laser_rx_unified/
- MSP3223/

Do not modify old demo directories, unrelated SDK examples, vendor code, or historical experimental folders unless explicitly instructed.

## Development Workflow

Before editing code:

1. Read the relevant files first.
2. Explain the current module responsibility or data flow.
3. Identify the minimum set of files that need modification.
4. Explain the risk of the change.
5. Make small, reviewable patches.

Do not rewrite entire files unless strictly necessary.

Do not make large architecture changes without explicit confirmation.

## Git Rules

Before any commit:

1. Run git status.
2. Run git diff --stat.
3. Review the actual diff.
4. Summarize modified files by purpose.
5. Propose a commit message.

Commit only when explicitly requested.

Use commit messages such as:

- feat: add new functionality
- fix: fix a bug
- refactor: restructure code without behavior change
- docs: update documentation
- chore: tooling or project maintenance
- checkpoint: save current working state

Never commit build outputs, temporary files, logs, or generated binaries.

## Laser Safety Rules

For laser-related code:

1. Any disconnect, abort, fatal parser error, or job cancellation must force laser off.
2. M5 must physically disable PWM/GPIO output.
3. Do not rely only on software modal state to indicate laser state.
4. RX should remain the source of truth for motion execution, job state, and laser state.

## WS63 / LiteOS Task Priority Rules

WS63 runs Huawei LiteOS-M. **Priority numbering is inverted compared to FreeRTOS:**

```text
Lower number = Higher priority
0 = HIGHEST (LOS_TASK_PRIORITY_HIGHEST)
31 = LOWEST (LOS_TASK_PRIORITY_LOWEST)
```

OSAL priority constants (from `osal_task.h`):

```text
OSAL_TASK_PRIORITY_ABOVE_HIGH   2
OSAL_TASK_PRIORITY_HIGH         3
OSAL_TASK_PRIORITY_BELOW_HIGH   4
OSAL_TASK_PRIORITY_ABOVE_MIDDLE 5
OSAL_TASK_PRIORITY_MIDDLE       6
...
```

Current task priorities (from `config.h`):

| Task           | Macro                    | Value | Priority   |
| -------------- | ------------------------ | ----- | ---------- |
| SLE stack      | `TASK_PRIO_SLE`          | 3     | HIGH       |
| UART RX        | `TASK_PRIO_JOB_UART`     | 3     | HIGH       |
| Job executor   | `TASK_PRIO_JOB_EXECUTOR` | 4     | BELOW_HIGH |
| Motion         | `TASK_PRIO_MOTION`       | 4     | BELOW_HIGH |

**Consequences:**

- SLE (3) has **higher** priority than job executor (4). SLE can always preempt exec when data arrives.
- `osal_yield()` on a lower-priority task (e.g. exec at 4) will NOT starve higher-priority tasks (e.g. SLE at 3). When SLE becomes Ready, it preempts exec immediately.
- If you need a task to wait for an event from a higher-priority task, use the project's existing LiteOS/OSAL synchronization primitives, such as event/semaphore/queue/task notification wrapper, not busy-wait with `osal_yield(); continue;`. Do not introduce FreeRTOS-specific APIs unless the SDK explicitly provides a compatibility layer.
- Never increase a task's priority number thinking it makes it "higher priority" — it does the opposite.

## SLE / Transport Rules

When modifying SLE transport logic:

1. Check both transmitter and receiver paths.
2. Preserve packet framing compatibility unless explicitly changing the protocol.
3. Consider duplicate packets, lost ACKs, credit/buffer overflow, timeout, and reconnect behavior.
4. Do not create two independent Grbl controllers.

## Host Tool Rules

For src/ws63_laser_host_ui/:

1. Keep Python scripts simple and inspectable.
2. Prefer clear logs over hidden automation.
3. Do not add large dependencies unless necessary.
4. Keep serial protocol behavior explicit.

## Screen Module Rules

For `src/deprecated/ws63_screen_st7796_ft6336/`:

1. This module is the deprecated historical WS63 screen self-test project. The project screen selection has moved from the old 4.0-inch ST7796 module to the `MSP3223/` reference package.
2. Keep it separate from `ws63_laser_single`, `ws63_laser_wifi`, and `ws63_laser_sle_job` unless the user explicitly asks for integration.
3. Current screen build enable switch is `CONFIG_ENABLE_SCREEN_SAMPLE=y`.
4. Current selected screen hardware:
   - Module: MSP3223 3.2-inch SPI touch display
   - LCD controller: ILI9341V, 240x320 RGB565
   - Touch controller: FT6336U, I2C address 0x38
   - Vendor/reference package: `MSP3223/`
   - Old reference package `src/4.0inch_SPI/` has been removed from the active project.
5. Current planned WS63 screen pins:

   | Function | GPIO |
   | -------- | ---- |
   | LCD_SCK  | GPIO7 |
   | LCD_MOSI | GPIO9 |
   | LCD_MISO | GPIO11 |
   | LCD_CS   | GPIO8 |
   | LCD_DC   | GPIO10 |
   | LCD_RST  | GPIO0 |
   | LCD_BL   | GPIO5 |
   | CTP_SCL  | GPIO16 / I2C1_SCL |
   | CTP_SDA  | GPIO15 / I2C1_SDA |
   | CTP_RST  | GPIO12 / held high |
   | CTP_INT  | GPIO13 |
   | SD_CS    | GPIO14 / reserved |

6. Screen verification order:
   - LCD first: verify red/green/blue/black color switching.
   - Touch second: verify FT6336 touch coordinate logs.
   - Product integration last: status display, parameter setting, and SLE central-control UI.
7. Avoid full-framebuffer designs unless memory is explicitly budgeted. For MSP3223, a full 240x320 RGB565 framebuffer is 153600 bytes; prefer line buffers, tile buffers, or partial refresh.
8. When modifying screen code, OpenCode/Codex should compile the screen firmware unless the user explicitly says not to compile.
9. Screen firmware archive path and build rules: see **Automation Scripts §3**.
10. Do not auto-flash the screen board. Burning remains a Win11 manual BurnTool step.
11. Do not use `src/4.0inch_SPI/` as an active reference. Use `MSP3223/init/ILI9341V_Init.txt`, `MSP3223/docs/driver_ic/ILI9341_Datasheet.pdf`, and the MSP3223 FT6336U documents instead.

## LVGL Module Rules

For `src/deprecated/ws63_screen_panel_lvgl/`:

1. This module is the archived LVGL v9.3.0 minimal port for the WS63 screen node. The target hardware is now MSP3223 (ILI9341V LCD + FT6336U touch); older ST7796 references in the code are temporary deprecated-driver references until the display driver is ported.
2. Keep it separate from laser modules. Do not integrate SLE/Host logic unless explicitly asked.
3. Current LVGL build enable switch is `CONFIG_ENABLE_LVGL_SAMPLE=y` (minimal port) or `CONFIG_ENABLE_LVGL_PANEL=y` (industrial panel UI).
4. `CONFIG_ENABLE_LVGL_SAMPLE` and `CONFIG_ENABLE_SCREEN_SAMPLE` are **mutually exclusive** (shared hardware). Only one can be enabled at a time.
5. LVGL source lives at `src/deprecated/ws63_screen_panel_lvgl/src/lvgl/` (v9.3.0). Do not modify LVGL core source unless patching a build warning; prefer port-layer changes.
6. LVGL config is `src/deprecated/ws63_screen_panel_lvgl/lv_conf.h`. Key settings:
   - `LV_COLOR_DEPTH=16`, `LV_COLOR_16_SWAP=1`
   - `LV_MEM_SIZE=40*1024` (40KB heap)
   - `LV_FONT_DEFAULT=&lv_font_montserrat_14`
   - Only label, button, bar widgets enabled
   - Only RGB565 draw support enabled
7. Display buffer should be sized for the active MSP3223 orientation after the ILI9341 port lands. The current 320x48 single RGB565 buffer is a legacy ST7796-era setting and should be reviewed during the MSP3223 migration.
8. Tick source: hardware timer (timer index 1, 1ms).
9. Task priority: 25 (low, non-critical UI task).
10. Screen firmware archive path reuses the screen name:
    - `/root/fbb_ws63/src/output/ws63/fwstage/latest/ws63-liteos-app_screen_all.fwpkg`
    - Do not create a separate `lvgl` firmware name; always replace `screen_all.fwpkg`.
11. LVGL demo page is `WS63 Laser Panel` with fake data. Do not connect to SLE/RX/Host unless explicitly asked.
12. When modifying LVGL port code, compile the screen firmware unless the user explicitly says not to compile.

## Unified RX Module Rules

For `src/ws63_laser_rx_unified/`:

1. This module is the new final RX firmware line for three transport modes: USART Direct, WiFi TCP, and SLE job packet.
2. Keep it separate from `ws63_laser_single`, `ws63_laser_wifi`, and `ws63_laser_sle_job` until the user explicitly asks for migration or integration.
3. Current build enable switch is `CONFIG_LASER_RX_UNIFIED=y`.
4. Current mainline is route-based integration, not the old shared `rx_stream` approach.
5. `rx_core/rx_stream.c` and `transports/uart_transport.c` are experimental Phase 2A prototype files. Do not use them as the three-mode integration mainline unless explicitly requested.
6. R4A compile-only boot validation passed. R4B starts only the route-local SLE Job RX, makes `RX_ROUTE_SLE_JOB` active, and reaches SLE server ready; Legacy UART/WiFi remain compiled but their transport tasks must not start. R4B-job TX + Host execution validation has passed repeatedly with the stable TX transmitter, `src/ws63_laser_host_ui`, and `preroll=4096`, including `@STATUS`, `@DATA_READY`, `DATA_RESUME`, `JOB_READY`, `EXEC_DONE`, and final laser OFF.
7. Later routes should reuse mature source behavior first:
   - Legacy UART route: `src/deprecated/ws63_laser_single/`
   - Legacy WiFi route: `src/deprecated/ws63_laser_wifi/`
   - SLE Job route: `src/ws63_laser_sle_job/receiver/`
8. Do not force USART/WiFi Grbl parsing and SLE job packet/cache parsing through one common parser.
9. LaserGRBL validation settings are route-specific. Do not use one streaming mode to evaluate every route. Legacy UART is not required to use Buffered and should be validated first with Grbl + UsbSerial + Synchronous. Legacy WiFi currently uses Grbl + Telnet + Buffered + Fast at `192.168.43.1:5000` as its fixed acceptance baseline. SLE Job does not use LaserGRBL and should continue using `src/ws63_laser_host_ui`.
10. Internal motion command compatibility headers should not be named `protocol.h`; use names such as `rx_motion_protocol.h` to avoid confusion with SLE `common/protocol.h`.
11. Unified RX firmware archive path and build rules: see **Automation Scripts §5**.
12. When modifying unified RX build integration or code, compile with `scripts/build_rx_unified_firmware.sh` unless the user explicitly says not to compile.
13. The build script switches `ws63_liteos_app.config`. To build TX/RX, screen, or LVGL afterward, run that target's own build script or explicitly reconfigure first; do not assume the previous sample selection is still active.
14. The integrated SLE Job route must keep its cache at 131072 bytes and preserve the stable protocol framing, CRC, ACK/NACK, sequence, duplicate, cache, and preroll behavior. Its Host demo preroll baseline remains 4096.
15. R5 has only two operational upper-level modes: `RX_MODE_GRBL_STREAM` and `RX_MODE_SLE_JOB`. R5A read-only mode/status query build and flash validation passed. With `SLE_JOB` active, `@STATUS`, `preroll=4096`, `EXEC_START`, `DATA_RESUME`, `JOB_READY`, `JOB_END`, and `EXEC_DONE` all passed, with final `active=0` and laser OFF. R5A implements snapshots and logs only; it did not add switching, an owner mechanism, or changes to the validated SLE protocol path.
16. `RX_MODE_GRBL_STREAM` should use one G-code parser, one processor, and one motion executor with UART and WiFi as simultaneous input frontends. Do not start two complete Legacy UART/WiFi execution cores together.
17. Do not add a GRBL owner mechanism for the current demo. The operator must not send UART and WiFi jobs concurrently; concurrent input behavior is unspecified. A simple response broadcast to UART and WiFi is acceptable for the demo.
18. R5B uses persistent SLE advertising. RX remains in `mode=SLE_JOB` and `active_route=SLE_JOB` indefinitely whether TX is connected or not; absence of TX must never trigger automatic WiFi or UART startup. R5B persistent SLE advertising validation passed: RX stayed in SLE Job past the former timeout, late TX connected, Host `@STATUS` passed, small job passed, final laser=OFF, WiFi/UART did not auto-start.
19. R5C is only a one-way manual switch from `SLE_JOB` to `LEGACY_WIFI`; do not describe it as the full `GRBL_STREAM` implementation. Host sends `@RX MODE=GRBL`, TX sends `PKT_ROUTE_SWITCH=0x15`, RX ACKs accepted first, then performs a delayed safe switch to Legacy WiFi. `route_switch_accepted` means the switch request passed the safe gate and was queued; it is not a Host-side proof that WiFi has already been validated by LaserGRBL.
20. R5C must not implement `LEGACY_WIFI -> SLE_JOB`, UART/WiFi dual frontends, owner/arbitration, or automatic fallback. R5D is the earliest phase for reverse switching.
21. R5D coexist work lives on `experiment/r5d-sle-wifi-coexist`. It is a demo experiment: start `SLE_JOB`, keep SLE advertising/server available, then start Legacy WiFi SoftAP/TCP as a coexist listener. The operator must not send SLE Host jobs and LaserGRBL jobs at the same time. R5D does not implement owner/arbitration, reverse switching, or the final shared UART/WiFi frontend.
22. Route switching commands must remain explicit. Do not use a single character or embed route control in ordinary SLE job data. R5C must send ACK/NACK before stopping SLE so the response can still reach TX/Host.
23. Any later mode switch must require IDLE, laser OFF, an empty motion queue, no SLE receive/execute operation, and no GRBL stream in progress. A failed switch must force the laser OFF and must not update the active mode incorrectly.
24. One first-integration attempt produced `@DATA_READY timeout / bad_begin`, then succeeded without code changes and remained non-reproducible across many later runs. Record it as an occasional initial handshake/state synchronization observation; do not change protocol framing, ACK/NACK semantics, TX, RX, or Host behavior unless reproducible evidence identifies a structural defect.
25. R4B checkpoint commit is `35501db4`. In later development, determine the current communication state from `mode` and `active_route`, not from `compiled_routes`; compiled routes only indicate code included in the firmware and do not imply runtime activation.
26. R5A must remain read-only. Do not add fallback, mode commands, route control packets, route switching, dual Grbl frontends, or owner behavior as part of R5A work.
27. `src/deprecated/ws63_screen_panel_lvgl/` is an archived screen workspace. Do not modify, stage, or commit it during unified RX work unless the user explicitly changes its scope.
28. Do not describe R5B as timeout fallback or as the final UART/WiFi dual-frontend Grbl Stream implementation. R5B starts no Grbl route: Legacy UART/WiFi remain compiled but stopped while SLE advertises persistently. Safe manual switching and the shared Grbl execution core remain R5C/R5D work.
29. Current SLE stop controls are not a hardware-level emergency stop. `@EXEC_STOP` / `PKT_EXEC_STOP` is a software safe stop, and `@ABORT` / `PKT_JOB_ABORT` aborts the job and clears cache. Do not call the current Host stop button a hard emergency stop. Product-grade emergency shutdown should be implemented as a hardware laser power cut.

## Development Environment

1. Firmware code editing and compilation happen in **WSL2**.
2. Project root: `/root/fbb_ws63`.
3. Firmware build commands:

   ```bash
   python3 build.py -c ws63-liteos-app menuconfig
   python3 build.py -c ws63-liteos-app -ninja -j24
   ```

   For normal TX/RX firmware builds, prefer `./scripts/build_sle_job_firmwares.sh --both` because it builds and archives TX/RX outputs separately. Use raw `build.py` only for menuconfig or low-level SDK debugging.

   For screen firmware builds, prefer `./scripts/build_screen_firmware.sh` because it switches to the correct config (`CONFIG_ENABLE_LVGL_SAMPLE=y`, `CONFIG_ENABLE_LVGL_PANEL=y`, or `CONFIG_ENABLE_SCREEN_SAMPLE=y`), disables all competing samples, builds, and archives the output. Use raw `build.py` only for menuconfig or low-level SDK debugging.

   For unified RX firmware builds, prefer `./scripts/build_rx_unified_firmware.sh`. It switches to `CONFIG_LASER_RX_UNIFIED=y`, builds, and archives the generated package as `src/output/ws63/fwstage/latest/ws63-liteos-app_rx_unified_all.fwpkg`.

   The SDK firmware output directory `src/output/ws63/fwpkg/ws63-liteos-app/` is shared and each build overwrites `ws63-liteos-app_all.fwpkg`. When building multiple firmware variants, the correct automated flow is serial: configure one variant, build it, immediately copy the generated package into `src/output/ws63/fwstage/latest/` with a function-specific name, then configure and build the next variant. Never assume multiple variants remain available in the raw `fwpkg/ws63-liteos-app/` output directory.

4. Do NOT move the firmware project to `/mnt/c/...` or Windows Desktop for compilation.
5. Host tool source (`src/ws63_laser_host_ui/`) can be edited in WSL2, but **running and serial debugging happen on Win11**.
6. Host tool is synchronized to the Win11 Desktop run directory using `scripts/sync_host_to_win.sh`. Manual copy is only a fallback when explicitly requested.
7. **Win11 COM 口不固定**，可能因 USB 口、拓展坞、线缆、驱动、插入顺序变化。不要假设 COM8/COM24/COM26 或 COM27/COM6/COM24 仍然有效，这些只能作为历史示例。
8. 每次新的运行/调试会话开始时，必须询问用户当前串口角色映射：
   - **TX 命令串口**：UART1，Host 发送 @BEGIN/@DATA/@EXEC_START/@STATUS
   - **TX 日志串口**：UART0 debug/log，TX 固件 osal_printk
   - **RX 日志串口**：UART0 debug/log，RX 固件 osal_printk
   Remember the mapping only for the current conversation/debug session. Do not implement persistent `host_config.json` unless the user explicitly asks for it again.
9. 当前安全波特率基线：
   - TX 命令串口：**115200**，除非用户明确说 UART1 已经重新编译并烧录为其他波特率；
   - TX 日志串口：**115200**；
   - RX 日志串口：**115200**。
10. Do NOT assume WSL2 can directly access COM ports.
11. All test instructions must distinguish:
    - **WSL2**: edit source, compile firmware, run scripts, check Git;
    - **Win11**: run Host, select COM ports, capture serial logs, upload G-code, verify execution.
12. 在给 Win11 运行/调试步骤前，先问：
    - 当前 TX 命令串口是哪个 COM？
    - 当前 TX 日志串口是哪个 COM？
    - 当前 RX 日志串口是哪个 COM？
    - 当前 TX 命令串口波特率是否仍为 115200？

## Automation Scripts / Common Workflow

### WSL2 / Win11 分工

| 环境      | 职责                                             |
| --------- | ----------------------------------------------- |
| **WSL2**  | 源码修改、固件编译、Host 上位机同步             |
| **Win11** | Host 运行、串口调试、BurnTool 烧录 |

- 不要把固件工程移动到 `/mnt/c/...` 下编译。
- 不要自动调用 BurnTool。
- 不要自动 commit。

### 1. Host 上位机同步脚本

**路径：** `/root/fbb_ws63/scripts/sync_host_to_win.sh`

**用途：** 将 WSL2 中的 Host 上位机源码同步到 Win11 桌面运行目录。

**WSL2 源码目录：** `/root/fbb_ws63/src/ws63_laser_host_ui/`

**Win11 运行目录：** `/mnt/c/Users/ZKX/OneDrive/Desktop/ws63_laser_host_ui/`

**使用命令：**

```bash
cd /root/fbb_ws63
./scripts/sync_host_to_win.sh
```

**Win11 启动命令：**

```cmd
cd /d C:\Users\ZKX\OneDrive\Desktop\ws63_laser_host_ui
python main.py
```

**说明：**

- 同步前会做 `main.py` 语法检查；
- `logs/` 目录不同步、不覆盖、不删除；
- Host 源码在 WSL2 中修改，实际运行在 Win11。

### 2. TX/RX 固件一键编译与归档脚本

**路径：** `/root/fbb_ws63/scripts/build_sle_job_firmwares.sh`

**用途：** 自动切换 TX/RX 配置，串行编译，且每次编译后立即归档到 `fwstage`，避免唯一 `ws63-liteos-app_all.fwpkg` 产物被下一次编译覆盖。

**使用命令：**

```bash
cd /root/fbb_ws63

# 编译 TX + RX（默认）
./scripts/build_sle_job_firmwares.sh --both

# 只编译 TX
./scripts/build_sle_job_firmwares.sh --tx-only

# 只编译 RX
./scripts/build_sle_job_firmwares.sh --rx-only
```

**归档目录：**

```text
/root/fbb_ws63/src/output/ws63/fwstage/latest/
```

**TX/RX 固件路径：**

```text
/root/fbb_ws63/src/output/ws63/fwstage/latest/ws63-liteos-app_tx_all.fwpkg
/root/fbb_ws63/src/output/ws63/fwstage/latest/ws63-liteos-app_rx_all.fwpkg
```

**关键规则：**

- `src/output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg` 是唯一临时产物；
- 多固件构建必须串行执行；
- 每个功能固件编译成功后必须立即复制到 `fwstage/latest/`；
- 归档文件必须按功能命名，例如 `tx`、`rx`、`screen`，不要直接依赖 raw fwpkg 目录里的文件。
- 脚本会自动关闭所有竞争 sample（`LASER_RX_UNIFIED`、`SCREEN_SAMPLE`、`LVGL_SAMPLE` 等），避免配置残留导致编译出错误固件。

### 3. 屏幕固件编译与归档脚本

**路径：** `/root/fbb_ws63/scripts/build_screen_firmware.sh`

**用途：** 自动切换到屏幕固件配置（LVGL、工业中控面板或自检页），关闭其它 app sample，编译并归档。

**使用命令：**

```bash
cd /root/fbb_ws63

# 编译 LVGL 屏幕固件（默认）
./scripts/build_screen_firmware.sh

# 明确编译 LVGL
./scripts/build_screen_firmware.sh --lvgl

# 编译工业中控面板 UI
./scripts/build_screen_firmware.sh --panel

# 编译原始自检页
./scripts/build_screen_firmware.sh --selftest
```

**归档路径：**

```text
/root/fbb_ws63/src/output/ws63/fwstage/latest/ws63-liteos-app_screen_all.fwpkg
```

**关键规则：**

- 脚本会自动关闭所有竞争 sample（`LASER_SLE_JOB_SAMPLE`、`LASER_RX_UNIFIED`、`LVGL_SAMPLE`/`SCREEN_SAMPLE` 互斥）；
- `--lvgl` 启用 `CONFIG_ENABLE_LVGL_SAMPLE=y`（LVGL v9.3.0 端口）；
- `--panel` 启用 `CONFIG_ENABLE_LVGL_PANEL=y`（工业中控面板 UI）；
- `--selftest` 启用 `CONFIG_ENABLE_SCREEN_SAMPLE=y`（当前为历史 ST7796 自检代码，后续需迁移到 MSP3223/ILI9341）；
- 归档文件名始终为 `ws63-liteos-app_screen_all.fwpkg`，不会因 variant 不同而改名。

### 4. Win11 BurnTool 烧录

- BurnTool 在 Win11 手动使用，不自动调用。
- `ws63-liteos-app_tx_all.fwpkg` 烧录到 TX 板。
- `ws63-liteos-app_rx_all.fwpkg` 烧录到 RX 板。
- `ws63-liteos-app_screen_all.fwpkg` 烧录到屏幕节点板。
- `ws63-liteos-app_rx_unified_all.fwpkg` 烧录到统一 RX 板。

### 5. Unified RX 固件编译与归档规则

**路径：** `/root/fbb_ws63/scripts/build_rx_unified_firmware.sh`

**用途：** 自动切换到 `CONFIG_LASER_RX_UNIFIED=y`，关闭其它 app sample，编译 unified RX，并把唯一 raw fwpkg 立即归档到 `fwstage`。

**使用命令：**

```bash
cd /root/fbb_ws63
./scripts/build_rx_unified_firmware.sh
```

**归档路径：**

```text
/root/fbb_ws63/src/output/ws63/fwstage/latest/ws63-liteos-app_rx_unified_all.fwpkg
```

**关键规则：**

- R4B 启用 `CONFIG_LASER_RX_TRANSPORT_SLE_JOB=y` 并默认激活 SLE Job route；
- Legacy UART/WiFi 继续编译用于 symbol coverage，但不得启动 UART RX、SoftAP 或 TCP server；
- integrated SLE job cache 固定为 131072 字节，Host demo preroll baseline 保持 4096；
- 脚本会关闭 `ENABLE_LASER_SINGLE_SAMPLE`、`ENABLE_LASER_WIFI_SAMPLE`、`ENABLE_LASER_SLE_JOB_SAMPLE`、`ENABLE_SCREEN_SAMPLE`、`ENABLE_LVGL_SAMPLE` 等竞争入口；
- 脚本会修改 `src/build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config`；
- 构建 TX/RX、screen、LVGL 或其它固件时，必须使用对应脚本重新切配置；
- 仍然遵守唯一 raw fwpkg 产物规则：构建成功后立即复制到功能命名的归档文件。

### 6. 常用开发流程

```bash
# 1. WSL2 修改 Host 上位机
vim /root/fbb_ws63/src/ws63_laser_host_ui/main.py

# 2. WSL2 同步 Host 到 Win11 桌面
./scripts/sync_host_to_win.sh

# 3. WSL2 编译 TX/RX 固件
./scripts/build_sle_job_firmwares.sh --both

# 4. Win11 烧录固件（手动 BurnTool）
#    TX: \\wsl.localhost\Ubuntu\root\fbb_ws63\src\output\ws63\fwstage\latest\ws63-liteos-app_tx_all.fwpkg
#    RX: \\wsl.localhost\Ubuntu\root\fbb_ws63\src\output\ws63\fwstage\latest\ws63-liteos-app_rx_all.fwpkg

# 5. Win11 启动 Host 并测试
cd /d C:\Users\ZKX\OneDrive\Desktop\ws63_laser_host_ui
python main.py
```

### 7. 手动调焦功能

**功能：** Host 手动控制激光调焦光开关。

**协议：**
- `@FOCUS_ON S{0-100}` → TX 转发 `PKT_FOCUS_CTRL(0x14)` → RX 开激光
- `@FOCUS_OFF` → TX 转发 `PKT_FOCUS_CTRL(0x14)` → RX 关激光
- TX 不直接控制激光，只转发命令
- RX 只在 IDLE 状态允许 FOCUS_ON
- FOCUS_OFF 无条件允许

**RX 安全互锁：**
- EXEC_START / EXEC_STOP / ABORT / disconnect / safe_stop / JOB_EXEC done 均强制 focus_off
- focus_force_off() 幂等

**功率换算：**
- Host S: 0-100
- RX internal: S × 10 = 0-1000
- Effective target power ratio is approximately S%; exact PWM mapping follows `laser_ctrl.c`.

**关键文件：**
- `common/protocol.h`: PKT_FOCUS_CTRL=0x14, focus_ctrl_payload_t
- `transmitter/main.c`: @FOCUS_ON/@FOCUS_OFF 解析
- `receiver/job_manager.c`: handle_focus_ctrl(), focus_force_off(), 安全互锁
- `main.py`: toggle_focus(), _focus_on(), _focus_off(), _focus_off_before_job()

Changing `PKT_FOCUS_CTRL` or `focus_ctrl_payload_t` requires rebuilding and flashing both TX and RX firmware.

### 8. Demo Stable Checklist

Before a demo run:

1. Confirm the current Win11 three-port serial role mapping.
2. Keep all serial ports at **115200**.
3. Verify `@STATUS` responds.
4. Verify manual focus on/off with `S10` and `S70`.
5. Run a short G-code job.
6. Run a large file with `preroll=4096`.
7. Confirm `EXEC_DONE` is reported.
8. After job done, confirm the laser is physically off.
9. Do not test the 921600 demo path unless the user explicitly requests it.

## Response Style

When working in this repository, prefer this structure:

1. Current problem
2. Root cause
3. Minimal fix
4. Affected files
5. Verification method
