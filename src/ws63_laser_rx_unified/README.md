# WS63 Laser RX Unified

This directory is the new unified RX firmware line for the WS63 laser marker.

Phase 2A adds the first real transport: USART Direct byte-stream input. It does
not replace the stable existing projects:

- `src/ws63_laser_single/`
- `src/ws63_laser_wifi/`
- `src/ws63_laser_sle_job/`

## Phase 1 Scope

- Add a standalone unified RX project directory.
- Reuse common hardware and execution modules.
- Initialize DAC, laser control, G-code processor, motion executor, safety, and
  RX core.
- Start only the motion executor task.
- Keep laser output physically off by default.
- Do not create UART/WiFi/SLE receive tasks.
- Do not implement runtime mode switching.
- Do not touch SLE packet/window/chunk/job cache logic.

## Phase 1C Build Status

- Build script: `scripts/build_rx_unified_firmware.sh`
- Status: script builds and archives the unified RX firmware.
- Archived firmware:

  ```text
  /root/fbb_ws63/src/output/ws63/fwstage/latest/ws63-liteos-app_rx_unified_all.fwpkg
  ```

- Manifest:

  ```text
  /root/fbb_ws63/src/output/ws63/fwstage/latest/manifest_rx_unified.txt
  ```

- Boot verification on hardware: passed in Phase 1D.

The script switches `ws63_liteos_app.config` to `CONFIG_LASER_RX_UNIFIED=y`,
disables competing app samples, enables only `CONFIG_LASER_RX_TRANSPORT_UART=y`,
keeps WiFi/SLE transports disabled, builds serially, and immediately archives
the single raw `ws63-liteos-app_all.fwpkg` output under the function-specific
unified RX filename.

## Phase 2A USART Direct

Goal:

```text
PC / serial terminal
-> RX UART
-> uart_transport
-> rx_stream
-> gcode_processor
-> motion_executor
-> laser_ctrl
```

Implemented:

- `transports/uart_transport.c/.h`: UART1 init, byte receive task, response write.
- `rx_core/rx_stream.c/.h`: shared stream line parser and Grbl-compatible responses.
- `rx_core_on_stream_byte(src, byte)`: common byte-stream entry for future WiFi reuse.
- Startup mode defaults to `RX_MODE_UART_DIRECT`.
- UART baud stays at the safe baseline: `115200`.

Not connected yet:

- WiFi TCP server.
- SLE job packet receiver.
- Runtime `MODE_SET`.
- TX or screen status mirroring.

Validation commands:

```bash
cd /root/fbb_ws63
bash -n scripts/build_rx_unified_firmware.sh
git diff --check
./scripts/build_rx_unified_firmware.sh
```

Hardware acceptance after flashing:

- Boot log contains `Phase 2A: USART Direct stream`.
- Boot log contains `[UART] rx task start`.
- Boot log contains `[RX_STATUS] mode=UART laser=OFF`.
- Serial terminal command `?` returns a status report.
- `$I`, `$G`, `$X` return reasonable Grbl-compatible responses.
- `M5` returns `ok` and keeps the laser physically OFF.
- `G0 X0 Y0` returns `ok`.
- 3-5 minutes idle/run smoke test has no reboot, assert, or hard fault.
- WiFi TCP and SLE job servers are not started.

Phase 2A hardware acceptance result:

- Startup and basic responses:
  - `WS63 Laser RX Unified`
  - `Grbl 1.1f ['$' for help]`
  - `$I` -> `[VER:1.1f.WS63_RX_UNIFIED:]` / `ok`
  - `$G` -> `[GC:G0 G54 G17 G21 G90 G94 ...]` / `ok`
  - `$X` -> `[MSG:Alarm lock cleared]` / `ok`
  - `$D` -> `motion busy=0 queue=0 abort=0 worker=1 laser=0` / `ok`
  - `?` -> `<Idle|MPos:0.000,0.000,0.000|FS:10000,0|Ln:1>`
- G-code and laser path:
  - `M5` -> `ok`, final `laser=OFF`
  - `G21` -> `ok`
  - `G90` -> `ok`
  - `G0 X0 Y0` -> `ok`
  - `M3 S50` + `G1 X0 F300` + `M5` -> laser path works.
  - Final `$D` reports `laser=0 power=0 pwm=0`.
- Conclusion:
  - UART1 business serial path passed.
  - `rx_stream` works correctly.
  - `gcode_processor` works correctly.
  - `motion_executor` worker is running correctly.
  - `laser_ctrl` path passed.
  - Final state: `laser=OFF`.
  - WiFi/SLE transports are not enabled.

## Reused Modules

- `executor/gcode_parser.*`: copied from the existing identical parser.
- `executor/gcode_processor.*`: copied from the existing identical processor.
- `hardware/dac8562.*`: copied from the existing DAC driver.
- `hardware/laser_ctrl.*`: copied from the existing laser PWM driver.
- `executor/motion_executor.*`: copied from the SLE receiver version because it
  has the most complete abort/clear-abort interface.

## Not Connected Yet

- WiFi TCP Grbl stream input.
- SLE job packet input.
- SLE job cache / job manager.
- Host UI status protocol.
- Screen node status protocol.

## Planned Phases

1. Phase 1: core skeleton and shared execution layer.
2. Phase 2: extract shared stream RX core, then connect USART Direct.
3. Phase 3: connect WiFi TCP to the shared stream RX core.
4. Phase 4: connect SLE job packet mode without changing the stable SLE packet
   protocol.
5. Phase 5: unify diagnostics/status output for Host UI and the screen node.
