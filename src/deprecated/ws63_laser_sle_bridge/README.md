# WS63 Laser SLE Bridge

This directory is the mainline transparent SLE serial bridge design.

## Role

`ws63_laser_sle_bridge` is intentionally separate from:

- `ws63_laser_single`: stable wired single-board baseline.
- `ws63_sle_laser`: experimental TX-side Grbl proxy / binary motion link.

## Architecture

```text
LaserGRBL
  -> COM8 UART
TX board: transparent UART <-> SLE byte bridge
  -> SLE
RX board: the only Grbl-compatible controller
  -> G-code parser / motion executor / DAC / laser control
```

The transmitter must not:

- print `Grbl 1.1f`.
- print `ok` or `error`.
- parse `$`, `G`, or `M` commands.
- fake `<Idle|...>` status.
- decide laser or motion state.

The receiver owns all Grbl semantics and responses.

## Stage 1

The current implementation is the minimal transparent bridge:

- TX: UART bytes to SLE write command.
- TX: SLE notify bytes to UART.
- RX: SLE payload bytes into `stream_io`.
- RX: `stream_io` reuses the single-board Grbl/G-code behavior.
- RX: SLE disconnect requests motion abort, flushes the queue, and forces laser off.

This stage is for short-command and LaserGRBL handshake validation, not long-file
stress testing.

## Stage 2

The next stage should add a reliable byte-stream transport under the same
transparent boundary:

- frame sequence.
- CRC.
- ACK and retry.
- duplicate suppression.
- sliding window.
- credit-based flow control.
- run-state heartbeat timeout with laser-off safety.

The reliable layer must not parse G-code or Grbl text. It only transports bytes.
