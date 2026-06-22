# WS63 Laser Host V2

PySide6 desktop host for the WS63 SLE Job laser engraving protocol. The four-page
interface mirrors `mockup.html` while keeping the serial protocol and worker-thread
implementation native to Python.

## Architecture

```text
main.py  →  MainWindow
  ├── app/                Core layer
  │   ├── event_bus.py    Qt signal bus for log/status events
  │   ├── config_store.py JSON config persistence
  │   ├── image_gcode.py  Bounded raster-to-G-code conversion
  │   └── state_models.py AppState, enums, RX state mapping
  ├── transports/         Protocol layer
  │   └── sle_tx_transport.py  SleJobSerialClient + SerialLogMonitor
  ├── workers/            Threading layer
  │   └── serial_worker.py     QThread wrappers for non-blocking ops
  └── ui/                 Presentation layer
      ├── main_window.py  Central controller
      ├── widgets/
      │   ├── status_badge.py  Color-coded status capsule
      │   └── sidebar_nav.py   Left navigation bar
      └── pages/
          ├── connection_page.py  Serial port management
          ├── gcode_page.py       Image workspace + G-code editor
          ├── job_page.py         Monitor, upload, exec, stop, abort, focus
          └── logs_page.py        Multi-channel log viewer
```

## Features

- **Connection**: TX command port + TX/RX debug log monitors
- **Image/G-code Workspace**: AI-provider hook, local image preview, bounded
  60×60 mm conversion, selectable scanline fill or closed-contour vector
  tracing, vector noise filtering/path simplification, original/effect preview
  switching, file loading, manual editing, and byte-size preview
- **Job Control**: Upload, preroll execution, exec start/stop, abort
- **Safety**: Software safe stop, job abort, manual focus laser on/off
- **Monitor**: upload byte progress, RX state, cache credit, and link status
- **Logs**: Filtered multi-channel log viewer with pause/resume
- **Settings**: JSON-persisted connection and job defaults
- **Responsive Desktop UI**: HTML-matched status strip, sidebar, card grid,
  fixed-view connection/task pages, and live task summary

## 状态与上传进度

- Host 不做自动 `@STATUS` 轮询；“查询状态”按钮只在用户手动点击时发送一次。
- 上传阶段按已接收字节显示下载进度。
- 执行阶段只显示任务处于执行中或完成，不显示执行行数进度。
- 正常完成优先由 RX 日志中的 `EXEC_DONE` 被动确认；停止或放弃不会误报为完成。

## Protocol

```text
PC Host  →  USB UART  →  TX Board  →  SLE  →  RX Board  →  Laser
```

Commands: `@BEGIN`, `@DATA_READY`, `@EXEC_START`, `@EXEC_STOP`, `@ABORT`,
`@STATUS`, `@FOCUS_ON/OFF`, and `@RX MODE=GRBL`.

`@EXEC_STOP` and `@ABORT` are software safety controls. They are not a
hardware emergency power cut.

The two upload actions are intentionally separate:

- **Upload only** sends the complete job without a preroll request and stops at
  `JOB_READY`.
- **Upload and execute** uses the configured preroll when it is non-zero; a zero
  preroll uploads the complete job before sending `EXEC_START`.

## Requirements

- Python 3.10+
- PySide6 >= 6.7
- pyserial >= 3.5

## Run

```bash
pip install -r requirements.txt
python main.py
```

## Test

```bash
python -m unittest discover -s tests -v
python -m compileall -q main.py app transports ui workers
```

The AI generation button exposes an integration hook but does not send prompts to
an external provider unless one is added. Local image import and G-code conversion
work without an external service.

Image conversion modes:

- **Scanline fill** thresholds the image and engraves horizontal dark runs. Use
  it when filled dark regions should be raster engraved.
- **Vector outline** traces black/white region boundaries into closed paths. The
  noise-area option removes small regions and path simplification reduces
  stair-step points. The converted preview shows these paths in red.

The square marking-size control applies the same `0–60 mm` side length to X and
Y. Source aspect ratio is preserved inside that square. A `0×0 mm` selection
creates a safe empty job containing only laser-off/end commands.

The task ring changes meaning by RX phase: it shows received bytes while the job
is downloading and executed G-code lines while marking. The Host counts the
actual prepared upload lines in advance. It polls `@STATUS` every 300 ms while
the RX is executing and every 1000 ms otherwise, whenever the command link and
job worker are idle.

Or with venv:

```bash
python -m venv .venv
.venv\Scripts\activate    # Windows
pip install -r requirements.txt
python main.py
```

## Sync to Win11

```bash
./scripts/sync_host_to_win.sh
```

Then run on Win11:

```cmd
cd C:\Users\ZKX\OneDrive\Desktop\ws63_laser_host_ui
python main.py
```
