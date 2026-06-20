# WS63 Laser RX Integrated

This directory is the future single-firmware RX integration line for the WS63
laser marker.

The integration direction changed after the Phase 2A experiment. The previous
approach tried to force USART Direct, WiFi TCP, and SLE job packet input through
one shared `rx_core/rx_stream` path. That prototype can boot and answer basic
USART commands, but LaserGRBL image jobs exposed compatibility and timing risks.

The new mainline is route-based integration:

- Legacy UART route: later reuse the mature `src/ws63_laser_single/` behavior.
- Legacy WiFi route: later reuse the mature `src/ws63_laser_wifi/` behavior.
- SLE Job route: later reuse the mature `src/ws63_laser_sle_job/receiver/`
  packet/cache/preroll behavior.
- Shared code is limited to firmware entry, route selection, route status,
  route-independent safety guards, build scripts, README, and manifest data.

Do not force these routes to share a Grbl stream parser or the SLE job
packet/cache state machine.

## R0/R1 Scope

R1 only creates the route-manager skeleton. It does not start any real route.

Implemented in R1:

- `mode_select/route_manager.c/.h`
- `mode_select/route_status.c/.h`
- `common/route_types.h`
- `routes/` placeholder directory
- `shared_safety/` placeholder directory
- boot log for route-based integration status
- laser forced OFF after hardware init

Not connected in R1:

- Legacy USART route
- Legacy WiFi route
- SLE job route
- LaserGRBL stream input
- SLE packet/cache input
- runtime Host route selection commands
- screen node integration

Expected R1 boot log:

```text
WS63 Laser RX Integrated
Route-based integration R1
[RX_INTEGRATED] active_route=NONE
[RX_INTEGRATED] compiled_routes=LEGACY_UART,LEGACY_WIFI,SLE_JOB
[RX_INTEGRATED] laser=OFF
[RX_INTEGRATED] no UART/WiFi/SLE route started
```

## R2A Scope

R2A compiles the prefixed Legacy UART route copied from
`src/ws63_laser_single/`, but still does not start the route.

Implemented in R2A:

- `routes/legacy_uart/legacy_uart_route.c/.h`
- `routes/legacy_uart/legacy_uart_gcode_parser.c/.h`
- `routes/legacy_uart/legacy_uart_gcode_processor.c/.h`
- `routes/legacy_uart/legacy_uart_motion_executor.c/.h`
- `routes/legacy_uart/legacy_uart_motion_protocol.h`
- `routes/legacy_uart/legacy_uart_config.h`
- route-local symbols are prefixed with `legacy_uart_`
- route-local headers avoid generic names such as `config.h` and `protocol.h`

Not connected in R2A:

- no UART RX task is created
- no `Grbl 1.1f` startup banner is sent
- no WiFi/SLE route is started

Expected R2A boot log:

```text
WS63 Laser RX Integrated
Route-based integration R2A
[RX_INTEGRATED] active_route=NONE
[RX_INTEGRATED] compiled_routes=LEGACY_UART,LEGACY_WIFI,SLE_JOB
[RX_INTEGRATED] laser=OFF
[RX_INTEGRATED] R2A legacy_uart compiled but not started
[RX_INTEGRATED] no UART/WiFi/SLE route started
```

R2A boot validation:

- Firmware flashed:
  `src/output/ws63/fwstage/latest/ws63-liteos-app_rx_unified_all.fwpkg`
- Required boot lines observed:
  - `WS63 Laser RX Integrated`
  - `Route-based integration R2A`
  - `active_route=NONE`
  - `compiled_routes=LEGACY_UART,LEGACY_WIFI,SLE_JOB`
  - `R2A legacy_uart compiled but not started`
  - `laser=OFF`
- Confirmed absent from boot log:
  - `Grbl 1.1f`
  - UART RX task start
  - `legacy_uart_route_init`
  - `legacy_uart_motion_executor_start_task`
  - WiFi TCP server start
  - SLE job server start
- Conclusion: Legacy UART route is compiled into the integrated RX firmware but
  remains inactive in R2A. No UART/WiFi/SLE route starts at boot, and the laser
  remains OFF.

## R2B Scope

R2B starts the prefixed Legacy UART route when
`CONFIG_LASER_RX_TRANSPORT_UART=y`. The route remains based on the mature
`src/ws63_laser_single/` behavior, with route-local symbols and headers to avoid
conflicts inside the integrated firmware.

Implemented in R2B:

- `legacy_uart_route_start()` initializes the Legacy UART G-code processor,
  motion executor, UART1 transport, UART RX task, and Legacy UART motion task.
- `route_manager_set_active(RX_ROUTE_LEGACY_UART)` validates that the laser is
  OFF and no active route is running before starting the route.
- `main.c` starts `RX_ROUTE_LEGACY_UART` automatically when the UART transport
  config is enabled.
- WiFi and SLE routes remain disabled and are not started.

Expected R2B boot log:

```text
WS63 Laser RX Integrated
Route-based integration R2B
[RX_INTEGRATED] R2B start legacy_uart route
[ROUTE] start LEGACY_UART
[LEGACY_UART] legacy_uart_route_init begin
[LEGACY_UART] legacy_uart_route_init OK uart=1 tx=GPIO15 rx=GPIO16 baud=115200
[LEGACY_UART] UART RX task start
[LEGACY_UART] route started
[RX_INTEGRATED] active_route=LEGACY_UART
[RX_INTEGRATED] laser=OFF
Grbl 1.1f ['$' for help]
```

Expected absent from R2B boot:

- WiFi TCP server start
- SLE job server start
- experimental `rx_stream` startup path

## R3A Scope

R3A compiles the prefixed Legacy WiFi route copied from
`src/ws63_laser_wifi/`, but does not start SoftAP, the TCP server, or a WiFi
task.

Implemented in R3A:

- `routes/legacy_wifi/legacy_wifi_route.c/.h`
- `routes/legacy_wifi/legacy_wifi_gcode_parser.c/.h`
- `routes/legacy_wifi/legacy_wifi_gcode_processor.c/.h`
- `routes/legacy_wifi/legacy_wifi_motion_executor.c/.h`
- `routes/legacy_wifi/legacy_wifi_motion_protocol.h`
- `routes/legacy_wifi/legacy_wifi_config.h`
- route-local symbols are prefixed with `legacy_wifi_`
- `legacy_wifi_motion_cmd_t` avoids sharing the old generic `motion_cmd_t`
- route-local headers avoid generic names such as `config.h` and `protocol.h`

Not connected in R3A:

- no SoftAP start
- no TCP server start
- no WiFi task creation
- no SLE job route
- no runtime Host route switching

Expected R3A boot log:

```text
WS63 Laser RX Integrated
Route-based integration R3A
[RX_INTEGRATED] R3A legacy_wifi compiled but not started
[RX_INTEGRATED] laser=OFF
```

Expected absent from R3A boot:

- SoftAP start
- TCP server start
- WiFi client connected
- `legacy_wifi_route_start`
- `legacy_wifi_route_task_entry`
- SLE job server start

R3A boot validation:

- Firmware flashed:
  `src/output/ws63/fwstage/latest/ws63-liteos-app_rx_unified_all.fwpkg`
- Required boot lines observed:
  - `WS63 Laser RX Integrated`
  - `Route-based integration R3A`
  - `compiled_routes=LEGACY_UART,LEGACY_WIFI,SLE_JOB`
  - `active_route=LEGACY_UART`
  - Legacy UART route starts normally.
  - `R3A legacy_wifi compiled but not started`
  - WiFi/SLE route not started.
  - `laser=OFF`
- Confirmed absent from boot log:
  - SoftAP start
  - TCP server start
  - WiFi client connected
  - `legacy_wifi_route_start`
  - `legacy_wifi_route_task_entry`
  - SLE job server start
- Conclusion: the prefixed Legacy WiFi route is linked into the integrated RX
  firmware for compile-only validation, but it is not started at boot. R3A
  keeps Legacy UART active, keeps WiFi/SLE stopped, and leaves the laser OFF.

## Route Validation Baselines

LaserGRBL settings are route-specific. Do not force one streaming mode across
USART, WiFi, and SLE validation.

Legacy UART route validation baseline:

- Protocol: Grbl
- Connection: UsbSerial
- Baudrate: 115200
- Streaming mode: Synchronous
- Use this for R2B stability validation before trying buffered image jobs.

Legacy WiFi route validation baseline:

- Protocol: Grbl
- Connection: Telnet
- SSID: `WS63_LASER_WIFI`
- Password: `12345678`
- Controller IP: `192.168.43.1`
- TCP port: `5000`
- Streaming mode: Buffered
- Thread mode: Fast
- Soft reset Ctrl-X: enabled
- DTR/RTS hard reset: disabled

SLE Job route validation baseline:

- SLE Job does not use LaserGRBL.
- Use `src/ws63_laser_sle_job_host` with TX command, TX log, and RX log serial
  ports selected for the current Win11 session.
- Preserve BEGIN/DATA/END/EXEC_START, ACK/NACK, job cache, preroll, and local RX
  execution behavior.

## R3-pre Legacy WiFi Route Audit

The independent WiFi sample is `src/ws63_laser_wifi/`.

Current source structure:

- `src/main.c`: independent app entry. Initializes `dac8562`, `laser_ctrl`,
  `gcode_processor`, `motion_executor`, `wifi_grbl_server`, creates the WiFi
  task, starts the motion executor task, and prints `[laser wifi] ready`.
- `src/wifi_grbl_server.c/.h`: SoftAP setup, TCP server, Grbl byte-stream
  handling, realtime `?`/`!`/`~`/Ctrl-X handling, `$` command responses, TCP
  disconnect cleanup, and LaserGRBL-facing responses.
- `src/gcode_parser.c/.h`: Grbl-style line parser.
- `src/gcode_processor.c/.h`: converts parsed G-code into local motion
  commands.
- `src/motion_executor.c/.h`: motion queue and DAC/laser execution worker.
- `src/dac8562.c/.h` and `src/laser_ctrl.c/.h`: independent hardware layer.
- `common/config.h`: WiFi, motion, geometry, task, DAC, and laser defaults.
- `common/protocol.h`: local `motion_cmd_t` and command IDs.

Independent WiFi connection parameters:

- SSID: `WS63_LASER_WIFI`
- Password: `12345678`
- Controller IP: `192.168.43.1`
- TCP port: `5000`
- SoftAP channel: `13`
- Hidden SSID flag: `1`
- TCP receive buffer: `512`
- TCP socket buffer: `8192`

Files that should be copied/adapted into `routes/legacy_wifi/` during R3A:

- `wifi_grbl_server.c/.h` as the route wrapper and TCP server basis.
- `gcode_parser.c/.h` with route-local names.
- `gcode_processor.c/.h` with route-local names.
- `motion_executor.c/.h` with route-local names.
- `common/config.h` as `legacy_wifi_config.h`.
- `common/protocol.h` as `legacy_wifi_motion_protocol.h`.

Files that should not be copied directly:

- `src/main.c`; integrated startup must remain in `src/ws63_laser_rx_unified/main.c`.
- Old WiFi project files in place; `src/ws63_laser_wifi/` remains the stable
  rollback baseline and must not be modified during route porting.
- Hardware layer should be evaluated carefully. Prefer reusing integrated
  `hardware/dac8562.c/.h` and `hardware/laser_ctrl.c/.h` if behavior remains
  identical; copy route-local hardware only if symbol or behavior isolation is
  required.

Expected symbol conflicts if copied without prefixing:

- `gcode_*`
- `gcode_processor_*`
- `motion_executor_*`
- `wifi_grbl_server_*`
- `task_wifi_grbl_entry`
- generic `config.h`
- generic `protocol.h`
- local `motion_cmd_t` command definitions if included through a generic name
- possible conflicts with existing `routes/legacy_uart/legacy_uart_*` if route
  local headers are not used consistently

Recommended R3A implementation:

- First implement `legacy_wifi compile-only`.
- Prefix copied route symbols:
  - `gcode_*` -> `legacy_wifi_gcode_*`
  - `gcode_processor_*` -> `legacy_wifi_gcode_processor_*`
  - `motion_executor_*` -> `legacy_wifi_motion_executor_*`
  - `wifi_grbl_server_*` and `task_wifi_grbl_entry` ->
    `legacy_wifi_route_*`
- Replace generic includes with route-local headers:
  - `legacy_wifi_config.h`
  - `legacy_wifi_motion_protocol.h`
  - `legacy_wifi_gcode_parser.h`
  - `legacy_wifi_gcode_processor.h`
  - `legacy_wifi_motion_executor.h`
  - `legacy_wifi_route.h`
- Do not start the WiFi route in R3A; only verify it compiles into the
  integrated firmware without symbol collisions.

Recommended R3B startup plan:

- Add route-manager activation for `RX_ROUTE_LEGACY_WIFI`.
- Start only when active route is `NONE` or `SAFE`, laser is OFF, motion is
  idle, and no other route is running.
- On success, print `active_route=LEGACY_WIFI` and start the WiFi TCP server.
- Validate LaserGRBL Telnet connection to `192.168.43.1:5000`.

R3B validation standard:

- LaserGRBL connects successfully using the WiFi validation baseline above.
- `$I`, `$G`, `$X`, `?`, `$D`, and `M5` respond correctly.
- A small rectangle job completes.
- Current image job completes in Buffered/Fast mode.
- Final state is laser physically OFF.
- WiFi, SLE, and UART routes do not preempt each other while a route is active.

R3 risk and rollback:

- Do not modify `src/ws63_laser_wifi/`.
- If R3 fails, burn the independent WiFi firmware and continue using the known
  working WiFi baseline.
- Do not modify SLE protocol, job cache, ACK/NACK, or Legacy UART R2B behavior.

## Experimental Stream Prototype

The following files are retained for reference only and are no longer the
three-mode integration mainline:

- `rx_core/rx_stream.c`
- `rx_core/rx_stream.h`
- `transports/uart_transport.c`
- `transports/uart_transport.h`
- `rx_core/rx_core.c`
- `rx_core/rx_core.h`

They belong to the experimental Phase 2A shared-stream prototype. They should
not be used as the basis for Legacy UART/WiFi/SLE integration unless explicitly
requested for a separate experiment.

## Planned Route Phases

1. R1: route manager skeleton, no real route.
2. R2A: compile prefixed Legacy UART route without starting it.
3. R2B: start the mature `ws63_laser_single` USART route and verify with
   LaserGRBL over wired serial.
4. R3A: compile prefixed Legacy WiFi route without starting it.
5. R3B: start the mature `ws63_laser_wifi` SoftAP TCP route and verify with
   LaserGRBL over WiFi.
6. R4: copy/adapt the mature `ws63_laser_sle_job/receiver` route and verify
   with `src/ws63_laser_sle_job_host`.
7. R5: add Host route query/select commands.
8. R6: clean up build scripts, README, and manifest fields.

## Build

Use the unified RX build script:

```bash
cd /root/fbb_ws63
./scripts/build_rx_unified_firmware.sh
```

The script switches `ws63_liteos_app.config` to `CONFIG_LASER_RX_UNIFIED=y`,
disables competing app samples, enables `CONFIG_LASER_RX_TRANSPORT_UART=y` for
R2B Legacy UART active validation, enables
`CONFIG_LASER_RX_TRANSPORT_WIFI=y` for R3A Legacy WiFi compile-only validation,
keeps SLE route transport disabled, builds serially, and archives the generated
firmware as:

```text
/root/fbb_ws63/src/output/ws63/fwstage/latest/ws63-liteos-app_rx_unified_all.fwpkg
```

## Safety Rules

- Boot must leave the laser physically OFF.
- Route switching must only be allowed when the active route is idle, laser is
  OFF, queue is empty, and no job is executing.
- R1 does not implement runtime route switching yet.
- Later route ports must preserve each source route's proven protocol behavior.

## Rollback

If any integrated route fails during later phases, burn the corresponding
stable independent firmware:

- USART Direct: `src/ws63_laser_single/`
- WiFi TCP: `src/ws63_laser_wifi/`
- SLE Job RX: `src/ws63_laser_sle_job/receiver/`
