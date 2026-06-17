# WS63 Laser SLE Job

Structured SLE job-transfer laser marker. This project is not a LaserGRBL
transparent serial bridge.

中文调试流程见：[调试任务书.md](调试任务书.md)

## Architecture

```text
PC host app
  -> USB UART custom job protocol
TX board
  -> SLE structured packets
RX board
  -> RAM job cache
  -> local G-code parsing
  -> motion executor / laser control
```

## Board Roles

- `LASER_SLE_JOB_TRANSMITTER`: reads PC UART commands, packages data, sends SLE packets.
- `LASER_SLE_JOB_RECEIVER`: receives packets, validates seq/offset/CRC, caches job, executes locally.

Current TX flow is intentionally two-stage:

```text
PC UART -> TX RAM cache -> SLE JOB_DATA packets -> RX RAM cache -> EXEC_START
```

TX does not send SLE packets while it is still receiving raw G-code bytes from the PC. This avoids blocking
the UART receive task with SLE ACK waits.

The task cache size is controlled by `CONFIG_LASER_SLE_JOB_CACHE_SIZE` in menuconfig. If that option is not
defined, the code falls back to 64KB.

Default config is RX. Switch role in:

```bash
python3 build.py -c ws63-liteos-app menuconfig
```

## Host UART Commands

All commands are ASCII lines except the raw G-code payload after `@BEGIN`.

```text
@BEGIN <job_id> <total_size> <crc16_hex>\n
wait for: @DATA_READY job=<job_id> size=<total_size>
<exact total_size bytes of G-code data>
wait for: @JOB_READY job=<job_id> size=<total_size>
@EXEC_START <job_id>\n
@EXEC_STOP\n
@ABORT\n
@STATUS\n
```

Example:

```text
@BEGIN 1 43 7a9c\n
wait @DATA_READY
G90
M3 S50
G1 X10 Y10 F1000
M5
wait @JOB_READY
@EXEC_START 1\n
```

The CRC is CRC16-CCITT, initial value `0xFFFF`, over the raw G-code bytes only.

## Packet Layer

Packet format is defined in `common/protocol.h`.

- `JOB_BEGIN`
- `JOB_DATA`
- `JOB_END`
- `JOB_ABORT`
- `EXEC_START`
- `EXEC_STOP`
- `STATUS_REQ`
- `STATUS_RESP`
- `ACK`
- `NACK`

Current version uses sequential `JOB_DATA` offsets. Out-of-order data returns NACK.

## Debug Logs

TX debug UART logs:

- `[JOB_TX_FRAME]`
- `[JOB_TX_ACK]`

RX debug UART logs:

- `[JOB_BEGIN]`
- `[JOB_DATA]`
- `[JOB_END]`
- `[EXEC_START]`
- `[JOB_SAFE_STOP]`
- `[JOB_EXEC]`

## Safety

RX immediately forces safe stop on:

- `EXEC_STOP`
- `JOB_ABORT`
- SLE disconnect
- execution parse/enqueue failure

Safe stop requests motion abort, clears queued motion, sends emergency command, and forces laser off.
