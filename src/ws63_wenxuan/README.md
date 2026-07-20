# WS63 Laser RX Integrated

This directory is the future single-firmware RX integration line for the WS63
laser marker.

The integration direction changed after the Phase 2A experiment. The previous
approach tried to force USART Direct, WiFi TCP, and SLE job packet input through
one shared `rx_core/rx_stream` path. That prototype can boot and answer basic
USART commands, but LaserGRBL image jobs exposed compatibility and timing risks.

The new mainline is route-based integration:

- Legacy UART route: later reuse the mature `src/deprecated/ws63_laser_single/` behavior.
- Legacy WiFi route: later reuse the mature `src/deprecated/ws63_laser_wifi/` behavior.
- SLE Job route: later reuse the mature `src/ws63_laser_sle_job/receiver/`
  packet/cache/preroll behavior.
- Shared code is limited to firmware entry, route selection, route status,
  route-independent safety guards, build scripts, README, and manifest data.

Do not force these routes to share a Grbl stream parser or the SLE job
packet/cache state machine.

## Phone control handoff

The HarmonyOS phone client connects directly to the RX SLE Job server using
the same SSAP service and packet protocol as the Host/TX path. The fixed-board
baseline admits only the whitelisted TX and Screen peers. The phone-enabled
defconfig additionally sets `CONFIG_LASER_RX_SLE_JOB_ALLOW_PHONE=y`, which
admits a non-fixed phone peer without changing the job executor, motion, laser,
or TX bridge code.

Control ownership remains single-writer: the first connected peer that writes
to the job data characteristic becomes `g_owner_conn_id`; writes from other
connected peers are dropped. Disconnecting the owner invokes the existing safe
stop callback. Build the phone-enabled RX package from the phone-integration
branch before testing the phone app.

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
`src/deprecated/ws63_laser_single/`, but still does not start the route.

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
`src/deprecated/ws63_laser_single/` behavior, with route-local symbols and headers to avoid
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
`src/deprecated/ws63_laser_wifi/`, but does not start SoftAP, the TCP server, or a WiFi
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

## R3B Scope

R3B starts the prefixed Legacy WiFi route when
`CONFIG_LASER_RX_TRANSPORT_WIFI=y`. The prefixed Legacy UART route may remain
compiled for symbol coverage, but `main.c` does not start the UART RX task in
R3B.

Implemented in R3B:

- `legacy_wifi_route_start()` initializes the Legacy WiFi G-code processor,
  motion executor, route wrapper, WiFi/TCP task, and Legacy WiFi motion task.
- `route_manager_set_active(RX_ROUTE_LEGACY_WIFI)` validates that the laser is
  OFF and no active route is running before starting the WiFi route.
- `main.c` starts `RX_ROUTE_LEGACY_WIFI` automatically when the WiFi transport
  config is enabled.
- Legacy UART is compile-only in this phase; SLE remains disabled and is not
  started.

Expected R3B boot log:

```text
WS63 Laser RX Integrated
Route-based integration R3B
[RX_INTEGRATED] R3B start legacy_wifi route
[ROUTE] start LEGACY_WIFI
[LEGACY_WIFI] legacy_wifi_route_init OK ssid=WS63_LASER_WIFI ip=192.168.43.1 port=5000 channel=13 hidden_flag=1
[LEGACY_WIFI] route started
[RX_INTEGRATED] active_route=LEGACY_WIFI
[RX_INTEGRATED] laser=OFF
[laser wifi] task started
[laser wifi] softap ssid=WS63_LASER_WIFI ip=192.168.43.1 port=5000 channel=13 hidden_flag=1
[laser wifi] tcp server listening port=5000
```

Expected absent from R3B boot:

- `LEGACY_UART UART RX task start`
- SLE job server start
- experimental `rx_stream` startup path

R3B Legacy WiFi route validation: passed.

Validated integrated configuration:

- Active route: `LEGACY_WIFI`
- Legacy UART: compiled but not started
- SLE route: not started
- SSID: `WS63_LASER_WIFI`
- Password: `12345678`
- Controller IP: `192.168.43.1`
- TCP port: `5000`
- SoftAP channel: `13`
- SSID mode: hidden (`hidden_flag=1`)
- LaserGRBL: Grbl + Telnet + Buffered + Fast
- Soft reset Ctrl-X: enabled
- DTR/RTS hard reset: disabled

Validated behavior:

- Integrated R3B boot completed with `active_route=LEGACY_WIFI`.
- SoftAP and TCP server started successfully.
- Win11 connected to `WS63_LASER_WIFI`.
- LaserGRBL connected to `192.168.43.1:5000` over Telnet and received the
  Grbl welcome response.
- `$I`, `$G`, `$X`, `?`, `$D`, and `M5` passed.
- Small rectangle job completed.
- Image job completed in Buffered/Fast mode.
- Final laser state was physically OFF.
- Legacy UART and SLE routes did not start or preempt the active WiFi route.

## R4A Scope

R4A compiles a route-local copy of the validated SLE Job RX implementation
from `src/ws63_laser_sle_job/receiver/` and its common packet code. It does not
start SLE advertising, initialize the SLE server, or create SLE/job execution
tasks.

Implemented in R4A:

- `routes/sle_job/sle_job_route.c/.h` compile-only lifecycle stub
- route-local server, job manager, 65536-byte cache, packet, CRC16, G-code,
  and motion executor sources
- exported symbols prefixed with `sle_job_`
- route-local headers named `sle_job_*.h`; no generic `config.h`, `protocol.h`,
  `packet.h`, or receiver component includes
- packet framing and state-machine behavior preserved from the stable SLE RX
- `CONFIG_LASER_RX_TRANSPORT_SLE_JOB=y` enables compile coverage

R4A protocol invariants:

- packet magic: `0xA55A`
- packet header: 10 bytes
- maximum payload: 224 bytes
- CRC16-CCITT initial value: `0xFFFF`
- packed structures and little-endian multi-byte fields remain unchanged
- ACK/NACK, sequence, duplicate, cache, and preroll behavior remain unchanged
- SLE job cache is fixed at 65536 bytes for the demo; Host upload rejects
  larger jobs before transfer
- Host demo preroll baseline remains 4096 bytes

Expected R4A boot log:

```text
WS63 Laser RX Integrated
Route-based integration R4A
[RX_INTEGRATED] compiled_routes=LEGACY_UART,LEGACY_WIFI,SLE_JOB
[RX_INTEGRATED] R4A sle_job compiled but not started
[RX_INTEGRATED] laser=OFF
```

R4A must not print SLE advertising/server/connection logs, initialize the job
manager, create `job_exec_task`, or process SLE job packets. The validated
Legacy WiFi route remains the active route during this compile-only phase.

R4A SLE Job compile-only boot validation: passed.

Validated boot state:

- `WS63 Laser RX Integrated` and `Route-based integration R4A` were printed.
- Compiled routes reported `LEGACY_UART,LEGACY_WIFI,SLE_JOB`.
- Active route remained `LEGACY_WIFI`; Legacy UART was compiled but not
  started.
- Legacy WiFi started normally with SSID `WS63_LASER_WIFI`, controller IP
  `192.168.43.1`, TCP port `5000`, channel `13`, and `hidden_flag=1`.
- TCP server reported listening on port `5000`.
- `R4A sle_job compiled but not started` was printed.
- Final boot laser state was `OFF`.

Confirmed absent from the boot log:

- SLE advertising or SLE server start
- `sle_job_route_server_init`
- SLE connection events
- `job_manager_init` or `job_exec_task created`
- `PKT_JOB_BEGIN`, `PKT_JOB_DATA`, or `PKT_EXEC_START`

Conclusion: the route-local SLE Job implementation is compiled into the R4A
build without starting any SLE runtime path. Legacy WiFi remains operational,
and the laser remains physically OFF.

## R4B Scope

R4B makes `RX_ROUTE_SLE_JOB` the default active route. Legacy UART and Legacy
WiFi remain compiled for coverage but do not start their UART, SoftAP, or TCP
tasks.

R4B startup flow follows the stable standalone SLE receiver:

1. Initialize the route-local G-code processor, motion executor, and job
   manager.
2. Register packet and disconnect callbacks with the route-local SLE server.
3. Start the route-local motion executor task.
4. Create the SLE initialization task, delay 500 ms for stack readiness, and
   initialize the SLE server.
5. Return from the app initialization callback without blocking. The SLE
   initialization task reports `[SLE_JOB_ROUTE] server ready` asynchronously.

The app initialization callback must not wait on an OSAL semaphore because it
runs in the SDK `osMain` initialization context rather than a normal waitable
task. Task-creation failures leave the route manager unchanged. Asynchronous
server initialization failures use the existing SLE safe-stop path and force
the laser OFF; `sle_job_route_is_idle()` remains false until the server is
ready.

Expected R4B boot state:

```text
WS63 Laser RX Integrated
Route-based integration R4B
[RX_INTEGRATED] legacy_uart compiled but not started in R4B
[RX_INTEGRATED] legacy_wifi compiled but not started in R4B
[RX_INTEGRATED] R4B start sle_job route
[ROUTE] start SLE_JOB
[SLE_JOB_ROUTE] route started, server init pending
[RX_ROUTE] active=SLE_JOB
[RX_INTEGRATED] active_route=SLE_JOB
[RX_INTEGRATED] laser=OFF
[SLE_JOB_ROUTE] server ready
```

R4B preserves the R4A protocol invariants and uses the configured SLE job cache. Hardware
acceptance uses the existing SLE TX firmware and
`src/ws63_laser_host_ui` with `preroll=4096`.

## R4B Job Execution Validation

R4B SLE Job active and job execution validation: passed.

Validated combination:

- TX: stable transmitter from `src/ws63_laser_sle_job`
- RX: integrated R4B firmware with `active_route=SLE_JOB`
- Host: `src/ws63_laser_host_ui`
- preroll: 4096 bytes

Validated behavior:

- TX and RX established and maintained the SLE connection.
- Host `@STATUS` completed successfully.
- Job upload and `@DATA_READY` completed successfully.
- The 4096-byte preroll path completed successfully.
- `DATA_RESUME`, `JOB_READY`, and `EXEC_DONE` completed successfully.
- Repeated job uploads and executions completed successfully after the first
  successful run.
- Final laser state was physically OFF.
- No persistent NACK storm, CRC mismatch, offset mismatch, cache overflow,
  hard fault, or assert was observed.

One initial integration attempt reported `@DATA_READY timeout / bad_begin`.
No code or configuration was changed before retrying, the retry succeeded, and
the condition did not recur during many subsequent successful runs. It is
recorded as a currently non-reproducible first-connection or first-job-begin
handshake/state synchronization event. It is not treated as a structural
protocol defect, and it does not justify changing the validated protocol, TX,
RX, or Host paths without reproducible evidence.

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
- SSID broadcast: hidden (`hidden_flag=1`)
- Streaming mode: Buffered
- Thread mode: Fast
- Soft reset Ctrl-X: enabled
- DTR/RTS hard reset: disabled

SLE Job route validation baseline:

- SLE Job does not use LaserGRBL.
- Use `src/ws63_laser_host_ui` with TX command, TX log, and RX log serial
  ports selected for the current Win11 session.
- Preserve BEGIN/DATA/END/EXEC_START, ACK/NACK, job cache, preroll, and local RX
  execution behavior.

## R3-pre Legacy WiFi Route Audit

The independent WiFi sample is `src/deprecated/ws63_laser_wifi/`.

Current source structure:

- `src/main.c`: independent app entry. Initializes `dac8563`, `laser_ctrl`,
  `gcode_processor`, `motion_executor`, `wifi_grbl_server`, creates the WiFi
  task, starts the motion executor task, and prints `[laser wifi] ready`.
- `src/wifi_grbl_server.c/.h`: SoftAP setup, TCP server, Grbl byte-stream
  handling, realtime `?`/`!`/`~`/Ctrl-X handling, `$` command responses, TCP
  disconnect cleanup, and LaserGRBL-facing responses.
- `src/gcode_parser.c/.h`: Grbl-style line parser.
- `src/gcode_processor.c/.h`: converts parsed G-code into local motion
  commands.
- `src/motion_executor.c/.h`: motion queue and DAC/laser execution worker.
- `src/dac8563.c/.h` and `src/laser_ctrl.c/.h`: independent hardware layer.
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
- Old WiFi project files in place; `src/deprecated/ws63_laser_wifi/` remains the stable
  rollback baseline and must not be modified during route porting.
- Hardware layer should be evaluated carefully. Prefer reusing integrated
  `hardware/dac8563.c/.h` and `hardware/laser_ctrl.c/.h` if behavior remains
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

- Do not modify `src/deprecated/ws63_laser_wifi/`.
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

## R5 Minimal Mode Design Draft

This section records the R5 direction. R5A and R5B are implemented. R5C adds
only the first manual one-way switch from SLE Job to Legacy WiFi. The full
shared Grbl Stream route, reverse switching, owner/arbitration, and fallback
timers are still not implemented.

R5 exposes only two upper-level modes:

- `RX_MODE_GRBL_STREAM`: UART listener and WiFi TCP server are both enabled as
  frontends to one shared G-code parser, G-code processor, motion executor, and
  laser control path.
- `RX_MODE_SLE_JOB`: the existing SLE packet, cache, preroll, and local job
  execution path used by the TX board and `src/ws63_laser_host_ui`.

### Grbl Stream Rules

- UART and WiFi may listen at the same time.
- The demo does not use `GRBL_OWNER_UART`, `GRBL_OWNER_WIFI`, or another owner
  mechanism.
- The operator is responsible for sending a task through only one frontend at
  a time. Concurrent UART and WiFi command input has unspecified behavior.
- Productization may add ownership or arbitration later if concurrent clients
  become a requirement.
- Grbl responses may initially use a simple broadcast strategy and be written
  to both UART and the connected WiFi client.

The implementation must not simply start the complete
`legacy_uart_route_start()` and `legacy_wifi_route_start()` paths together.
Those routes each contain their own parser, processor, and executor. A later
R5 implementation must instead create or refactor one GRBL Stream route with:

- one G-code parser
- one G-code processor
- one motion executor
- one UART frontend
- one WiFi TCP frontend

### Boot And Switching Draft

R5B boots into SLE Job and keeps SLE advertising active indefinitely. A late
TX must still be able to connect, so absence of a TX never triggers automatic
fallback. Legacy UART and Legacy WiFi remain compiled but stopped.

Explicit switching paths:

- GRBL Stream to SLE Job: accept a complete command such as `@RX MODE=SLE`
  from either UART or WiFi. Do not use a single-character command. This is not
  implemented in R5C.
- SLE Job to Legacy WiFi: R5C uses Host command `@RX MODE=GRBL`, TX packet
  `PKT_ROUTE_SWITCH=0x15`, and target `LEGACY_WIFI`. Do not place route-control
  characters inside ordinary SLE job data.

No switch is allowed while work is active. The minimum safe switch gate is:

- controller state is IDLE
- laser is physically OFF
- motion queue is empty
- SLE is not RECEIVING or EXECUTING
- Grbl is not streaming

On any failed switch, the implementation must call `laser_force_off()` and
must not update the active mode unless the target mode started successfully.

## R5A Read-Only Mode Status

R5A adds status expression and logging only. It does not implement runtime
mode switching, automatic fallback, the dual Grbl frontend, or an owner.

The upper-level mode and lower-level route have different meanings:

- `RX_MODE_NONE`: no operating mode is active.
- `RX_MODE_GRBL_STREAM`: future shared Grbl execution core with UART and WiFi
  input frontends. A Legacy UART or Legacy WiFi route maps to this mode.
- `RX_MODE_SLE_JOB`: validated SLE job packet/cache/preroll execution mode.
- `active_route` identifies the concrete route currently running.
- `compiled_routes` only identifies code included in the firmware and does not
  imply that a route is active.

R5A provides a read-only status snapshot containing mode, active and
recommended routes, compiled route flags, laser state, conservative busy and
switchable state, switch block reason, and switch count. If route idleness
cannot be established, the snapshot reports busy with `UNKNOWN_BUSY` instead
of reporting that switching is safe.

R5A boot still starts only the validated SLE Job route. Legacy UART and Legacy
WiFi remain compiled but stopped. The SLE protocol, cache, ACK/NACK, sequence,
preroll, TX compatibility, and Host job path are unchanged.

R5A mode/status query build and flash validation passed. With `SLE_JOB` active,
the existing path still completed `@STATUS`, `preroll=4096`, `EXEC_START`,
`DATA_RESUME`, `JOB_READY`, `JOB_END`, and `EXEC_DONE`. The final status
reported `active=0`, and the laser was physically OFF. R5A introduced no
switching, no owner mechanism, and no SLE protocol-path changes.

Expected additional boot logs:

```text
[RX_MODE] mode=SLE_JOB route=SLE_JOB recommended=SLE_JOB laser=OFF busy=1 switchable=0 reason=ROUTE_BUSY switches=1
[RX_MODE] compiled=UART,WIFI,SLE_JOB
[RX_MODE] grbl_stream=planned uart_wifi_dual_frontend owner=disabled
```

The initial status can conservatively report `ROUTE_BUSY` while asynchronous
SLE server initialization is still pending. Later callers of
`route_manager_get_status_snapshot()` receive the current route state.

## R5B Persistent SLE Advertising

R5B starts the validated SLE Job route and keeps it active whether or not a TX
is currently connected. After the server becomes ready, a low-priority monitor
reports connection-state changes but never stops SLE and never starts another
route. This lets TX and Host come online at any later point in the demo.

The former 8000 ms automatic WiFi fallback was removed after hardware testing:
RX could leave SLE Job before TX/Host completed startup, leaving TX disconnected
and causing Host `@DATA_READY` timeouts. Increasing the timeout would only hide
that lifecycle mismatch, so R5B now uses persistent SLE standby instead.

Legacy UART and Legacy WiFi remain compiled but stopped. R5B does not implement
WiFi fallback, the UART/WiFi dual frontend, an owner mechanism, manual mode
commands, or a SLE route-switch packet. Packet framing, CRC, job cache,
ACK/NACK, sequence, duplicate, and preroll behavior are unchanged.

Expected persistent standby logs:

```text
[RX_BOOT] policy=SLE_PERSISTENT_ADVERTISING
[RX_BOOT] sle advertising persistent, waiting tx indefinitely
[RX_MODE] mode=SLE_JOB route=SLE_JOB tx_connected=0 laser=OFF ...
[RX_BOOT] tx_connected=1 stay SLE_JOB
[RX_MODE] mode=SLE_JOB route=SLE_JOB tx_connected=1 laser=OFF ...
```

## R5B Persistent SLE Advertising Validation

R5B persistent SLE advertising validation: passed.

Validated behavior:

- RX booted with `policy=SLE_PERSISTENT_ADVERTISING`.
- RX remained in `mode=SLE_JOB` and `active_route=SLE_JOB` beyond the former
  8000 ms timeout without falling back to WiFi or UART.
- TX connected after the original timeout window and established the SLE link.
- Host `@STATUS` completed successfully.
- Small job upload and execution completed successfully.
- Final laser state was physically OFF.
- Legacy WiFi and Legacy UART did not start automatically at any point.

## R5C Manual SLE Job To Legacy WiFi Switch

R5C implements only one direction:

```text
SLE_JOB -> LEGACY_WIFI
```

It does not implement reverse switching, the future UART/WiFi dual frontend,
an owner mechanism, or automatic fallback.

Control path:

1. Host button sends `@RX MODE=GRBL`.
2. TX parses the command without entering data mode.
3. TX sends `PKT_ROUTE_SWITCH=0x15` with a 4-byte payload:
   `target_route=LEGACY_WIFI`, `flags=0`, `reserved=0`.
4. RX verifies safe idle state while SLE is still connected.
5. RX sends ACK first, then starts a delayed switch task.
6. The delayed switch stops SLE advertising/connection, starts Legacy WiFi, and
   updates mode/route to `GRBL_STREAM` / `LEGACY_WIFI`.

The ACK text `route_switch_accepted` means the request passed the safe gate and
the delayed switch was queued. It is not a guarantee that Host has already
validated the WiFi server. After acceptance, the operator manually connects
LaserGRBL to:

- SSID: `WS63_LASER_WIFI`
- Password: `12345678`
- Address: `192.168.43.1`
- Port: `5000`
- LaserGRBL mode: Grbl + Telnet + Buffered + Fast

R5C safe gate:

- active route is `SLE_JOB`
- target route is `LEGACY_WIFI`
- job manager is idle
- no receiving or executing job is active
- SLE motion executor is not busy
- SLE motion queue is empty
- laser is physically OFF
- no route switch is already in progress

Expected R5C logs:

```text
Route-based integration R5C
[ROUTE_SWITCH] request from=SLE_JOB target=2 ...
[ROUTE_SWITCH] safe idle check passed
[ROUTE_SWITCH] ack accepted, delayed switch start
[ROUTE_SWITCH] stop SLE_JOB
[ROUTE_SWITCH] start LEGACY_WIFI
[RX_MODE] mode=GRBL_STREAM route=LEGACY_WIFI
```

## R5D SLE + WiFi Coexist Demo Experiment

R5D is an experiment on branch `experiment/r5d-sle-wifi-coexist`. It keeps the
R5C code available but changes the boot policy for a demo-friendly coexistence
test:

- boot starts `SLE_JOB`
- SLE server remains advertising/connectable as `sle_job_rx`
- after SLE server readiness, Legacy WiFi starts as a coexist listener
- WiFi SoftAP remains `WS63_LASER_WIFI`
- TCP server remains `192.168.43.1:5000`
- Legacy UART remains compiled but stopped
- laser is forced OFF at boot and before route startup

R5D is not the final `GRBL_STREAM` architecture. It does not implement owner
or arbitration, does not merge UART/WiFi frontends, and does not implement
reverse switching. During the experiment, the operator must ensure only one
upstream is actively sending a job at a time:

- SLE Host through TX, or
- LaserGRBL through WiFi/Telnet

Concurrent SLE and WiFi job input is unsupported and has unspecified behavior.

R5D hardware validation passed:

- SLE connected normally.
- SLE 23 KB job executed successfully.
- WiFi `WS63_LASER_WIFI` was visible.
- LaserGRBL Telnet `192.168.43.1:5000` executed a TCP marking job.
- After disconnecting WiFi, returning to the SLE Host and running another
  23 KB SLE job still worked.
- Final laser state was OFF.
- No hard fault or assert was observed.

### SLE Stop and Abort Semantics

**详细的停止/中止语义请参见 AGENTS.md Unified RX Module Rules 第29条。**

简要说明：
- `@EXEC_STOP`：软件安全停止
- `@ABORT`：中止任务并清除缓存
- 产品级急停应为硬件激光电源切断

Expected R5D logs:

```text
Route-based integration R5D
[RX_BOOT] policy=SLE_WIFI_COEXIST_DEMO
[ROUTE] start SLE_JOB
[SLE_JOB_ROUTE] server ready
[RX_BOOT] start WiFi coexist listener
[ROUTE] start LEGACY_WIFI coexist
[RX_MODE] primary=SLE_JOB wifi_coexist=1 laser=OFF
[laser wifi] softap ssid=WS63_LASER_WIFI ...
[laser wifi] tcp server listening port=5000
```

## Planned Route Phases

1. R1: route manager skeleton, no real route.
2. R2A: compile prefixed Legacy UART route without starting it.
3. R2B: start the mature `ws63_laser_single` USART route and verify with
   LaserGRBL over wired serial.
4. R3A: compile prefixed Legacy WiFi route without starting it.
5. R3B: start the mature `ws63_laser_wifi` SoftAP TCP route and verify with
   LaserGRBL over WiFi.
6. R4A: compile a route-local copy of the mature SLE Job RX without starting it.
7. R4B boot: activate SLE Job and reach SLE server ready. This boot milestone
   is complete.
8. R4B-job: TX + Host SLE job execution acceptance passed using the 4096-byte
   preroll demo baseline, including repeated successful executions and final
   laser OFF verification.
9. R5A: read-only mode status/query reporting passed build, flash, and SLE Job
   execution validation without changing the R4B startup or execution path.
10. R5B: keep SLE Job advertising persistently with no automatic fallback.
11. R5C: implement safe idle switching from SLE Job to the future Grbl Stream
    route.
12. R5D: implement safe idle switching from Grbl Stream back to SLE Job.
13. R5E: expose the validated mode controls and status to the screen UI.
14. R6: clean up build scripts, README, and manifest fields.

## Build

Use the unified RX build script:

```bash
cd /root/fbb_ws63
./scripts/build_rx_unified_firmware.sh
```

The script switches `ws63_liteos_app.config` to `CONFIG_LASER_RX_UNIFIED=y`,
disables competing app samples, enables `CONFIG_LASER_RX_TRANSPORT_UART=y` for
Legacy UART compile coverage, enables `CONFIG_LASER_RX_TRANSPORT_WIFI=y` for
the validated Legacy WiFi route, and enables
`CONFIG_LASER_RX_TRANSPORT_SLE_JOB=y` for the R5B persistent route. The SLE job
cache is fixed at 65536 bytes. Legacy UART/WiFi remain compiled and R5D starts
WiFi as a coexist listener while SLE_JOB remains primary. The script builds
serially and archives the generated
firmware as:

```text
/root/fbb_ws63/src/output/ws63/fwstage/latest/ws63-liteos-app_rx_unified_all.fwpkg
```

## Safety Rules

- Boot must leave the laser physically OFF.
- Route switching must only be allowed when the active route is idle, laser is
  OFF, queue is empty, and no job is executing.
- Runtime mode switching is not implemented yet; R5B remains in SLE Job.
- Later route ports must preserve each source route's proven protocol behavior.

## Rollback

If any integrated route fails during later phases, burn the corresponding
stable independent firmware:

- USART Direct: `src/deprecated/ws63_laser_single/`
- WiFi TCP: `src/deprecated/ws63_laser_wifi/`
- SLE Job RX: `src/ws63_laser_sle_job/receiver/`
