# WS63 Single Board Laser Marker

This sample is a single-board reproduction of the Arduino laser marker reference under `src/ws63_test/arduino激光打标机源码`.

It intentionally removes the transmitter/receiver split and all SLE wireless code:

```text
LaserGRBL
  -> UART1 / G-code
  -> WS63 single board
      -> G-code parser
      -> local motion executor
      -> DAC8562 X/Y
      -> PWM laser power
      -> ok after local execution
```

## Scope

- Compatible with LaserGRBL-style `Grbl 1.1f` handshake.
- Handles `?`, `$I`, `$G`, `$X`, `$H`, `$C`, `G0`, `G1`, `G90`, `G91`, `G92`, `M3`, `M4`, `M5`, `S`, and `F`.
- Replies `ok` only after the local board has parsed and executed the command.
- Uses one WS63 board only. There is no SLE client, SLE server, ACK sequence, or remote command queue.

## Hardware Pins

The default pins follow the existing `ws63_test` receiver/transmitter wiring:

| Function | GPIO | Peripheral |
| --- | ---: | --- |
| UART TX | GPIO15 | UART1_TXD |
| UART RX | GPIO16 | UART1_RXD |
| DAC SCK | GPIO7 | SPI0_SCK |
| DAC MOSI | GPIO9 | SPI0_OUT |
| DAC CS | GPIO10 | GPIO output |
| Laser PWM | GPIO2 | PWM2 |

## Build Selection

Enable `ENABLE_LASER_SINGLE_SAMPLE` from `application/samples/Kconfig`. This sample selects SPI master support and then builds `src/ws63_laser_single`.

Use this project first when validating the short Arduino-style chain. After it works, the wireless split in `ws63_test` can be compared against it to isolate SLE or flow-control problems.

