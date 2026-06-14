# WS63 Wireless Laser Marker

This directory is the isolated wireless development tree. It intentionally does
not modify `src/ws63_laser_single`, which is the current stable wired
single-board baseline.

## Goal

Build the wireless version in small, reversible steps:

```text
LaserGRBL / PC
  -> UART
  -> transmitter WS63
  -> SLE / NearLink
  -> receiver WS63
  -> proven motion queue + DAC8562 + PWM laser chain
```

## Current Step

The isolated wireless tree now contains both board roles:

- `receiver/`: SLE server named `LaserRX`; receives wireless motion packets and
  feeds the proven motion queue, DAC8562 galvo output, and PWM laser chain.
- `transmitter/`: UART-facing LaserGRBL bridge; parses the common G-code subset
  and forwards motion packets to `LaserRX` over SLE.
- `common/`: shared wireless packet format, CRC, and fixed build-time settings.

The first wireless bring-up target is link validation with laser power disabled:

```text
LaserGRBL / PC
  -> UART
  -> transmitter WS63
  -> SLE / NearLink
  -> receiver WS63
  -> motion queue, DAC8562, PWM laser chain
```

The transmitter currently uses conservative per-command ACK flow control. This
is acceptable for initial link testing; high-throughput engraving should later
move to a sliding-window sender with queue-watermark backpressure.

## Reference Wireless Code

Use these mature reference files from `src/ws63_test` when adding SLE:

- `src/ws63_test/transmitter/sle_client.c`
- `src/ws63_test/transmitter/sle_client.h`
- `src/ws63_test/receiver/sle_server.c`
- `src/ws63_test/receiver/sle_server.h`
- `src/ws63_test/common/protocol.h`
- `src/ws63_test/common/crc16.c`
- `src/ws63_test/common/crc16.h`

Do not move them directly into the stable single-board tree. Port the needed
pieces into this directory first, then test the receiver and transmitter roles
separately.

`common/protocol.h` is the shared command/status contract for the wireless
transport. The transmitter sends `motion_cmd_t` directly, and the receiver
validates the packet then enqueues the same structure into the motion executor.
`common/wireless_crc16.*` only provides CRC helpers for that shared contract.

## Build Selection

Enable `ENABLE_LASER_WIRELESS_SAMPLE` from `application/samples/Kconfig`.
Select exactly one role:

- `LASER_WIRELESS_RECEIVER`
- `LASER_WIRELESS_TRANSMITTER`

Keep `ENABLE_LASER_SINGLE_SAMPLE` disabled while building this target, because
the samples CMake file selects one laser sample at a time.

## LiteOS Task Priority Rule

LiteOS task priorities use smaller numbers for higher priority. For this
project, keep SLE callbacks above motion execution so wireless control packets
such as `M5` are not delayed by long movement segments:

```c
#define TASK_PRIO_SLE    2
#define TASK_PRIO_UART   3
#define TASK_PRIO_MOTION 4
```

Do not raise a task priority by increasing the number; that lowers its priority
on LiteOS.
