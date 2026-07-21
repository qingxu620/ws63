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
  │   ├── image_gcode.py  Raster/vector image-to-G-code conversion
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
- **Image/G-code Workspace**: AI-provider hook, local image import and integrated
  crop/transform preview, 99×99 mm work-area validation, grayscale PWM,
  nine dithering algorithms, continuous centerline and closed-contour tracing,
  vector fill, manual G-code editing, and byte-size preview
- **Job Control**: Upload, preroll execution, exec start/stop, abort
- **Safety**: Software safe stop, job abort, manual focus laser on/off
- **Monitor**: upload byte progress, RX state, cache credit, and link status
- **Logs**: Filtered multi-channel log viewer with pause/resume
- **Settings**: JSON-persisted connection and job defaults
- **Responsive Desktop UI**: HTML-matched status strip, sidebar, card grid,
  fixed-view connection/task pages, and live task summary

## 状态与上传进度

- Host 在执行阶段通过 TX 命令串口低频轮询 `@STATUS`，因此只连接 TX
  命令串口也能检测正常完成；“查询状态”按钮仍可手动发送一次状态查询。
- 上传阶段按已接收字节显示下载进度。
- 执行阶段只显示任务处于执行中或完成，不显示执行行数进度。
- `EXEC_START` 确认后立即释放串口命令 worker；执行期间仍可查询状态、软件停止或放弃任务。
- 正常完成可由 RX 日志中的 `EXEC_DONE` 被动确认，也可由执行期 `@STATUS`
  轮询观察到 RX 回到 IDLE 后确认；停止或放弃不会误报为完成。

## Protocol

```text
PC Host  →  USB UART  →  TX Board  →  SLE  →  RX Board  →  Laser
```

Commands: `@BEGIN`, `@DATA_READY`, `@EXEC_START`, `@EXEC_STOP`, `@ABORT`,
`@STATUS`, `@FOCUS_ON/OFF`, and `@RX MODE=GRBL`.

Before every upload, Host sends the reserved ASCII CAN byte (`0x18`). TX
recognizes it outside both command and raw-data parsing, aborts any prior RX job,
clears its local upload transaction, and returns `@OK resync rx=aborted`. Host
does not send `@BEGIN` until that acknowledgement arrives. The G-code validator
rejects `0x18` so it cannot be confused with payload data.

`@EXEC_STOP` and `@ABORT` are software safety controls. They are not a
hardware emergency power cut.

The two upload actions are intentionally separate:

- **Upload only** sends the complete job without a preroll request and stops at
  `JOB_READY`.
- **Upload and execute** sends an RX auto-start threshold when preroll is
  non-zero, so DATA remains continuous while RX starts execution locally. A
  zero preroll uploads the complete job before sending `EXEC_START`. Host keeps
  the legacy preroll handshake only when TX does not advertise RX auto-start
  support in `@DATA_READY`. For the RX auto-start path, the Host task display
  switches to execution state as soon as the TX cumulative offset reaches the
  preroll threshold, while the remaining DATA continues uploading in the
  background.

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

- **Scanline fill** performs true 8-bit grayscale PWM over the selected S-min to
  S-max range. It supports horizontal, vertical, and 45° paths, uni/bidirectional
  motion, optional binary thresholding, optional G0 fast travel, and 1–50 lines/mm.
- **1-bit dithering** provides Floyd–Steinberg, Atkinson, Burkes,
  Jarvis–Judice–Ninke, Random, Sierra 2/3/Lite, and Stucki choices.
- **Centerline** thins the subject and emits continuous 8-connected strokes
  instead of horizontal raster fragments.
- **Vector outline** traces black/white region boundaries into closed paths. The
  noise-area option removes small regions and path simplification reduces
  stair-step points. The converted preview shows these paths in red.
- **LaserGRBL-style independent vector** follows LaserGRBL's practical workflow:
  tune brightness/contrast, clip near-white background pixels, remove small
  spots, smooth the path, then emit RX-compatible G1 outline G-code. It is not
  Potrace and does not claim Potrace-equivalent curves.
- The image workspace keeps the main panel compact. `图像参数...` opens the
  integrated import dialog for crop, auto trim, rotate, flip, invert, four
  grayscale formulas, threshold, LaserGRBL-compatible tone controls, scan
  quality/direction, spot removal, smoothing, path optimization, downsampling,
  and fill.
  `目标参数...` opens the target/laser dialog for M3/M4, speed, S range, DPI,
  width/height, aspect lock, and XY offset.

See `THIRD_PARTY_NOTICES.md` for the LaserGRBL/Potrace reference and license
notes behind the vectorization workflow.

Image conversion does not truncate or reject generated G-code by byte size.
The firmware-cache size audit belongs only to the separate **upload only**
operation; upload-and-execute retains its streaming/preroll behavior.

The square marking-size control applies the same `0–99 mm` side length to X and
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

## Build the Windows directory edition

Run on Windows 11:

```cmd
build_windows.bat
```

The script installs the build-only requirements, regenerates the multi-size
Windows icon, creates the PyInstaller one-folder bundle, and runs a packaged UI
startup plus serial-port enumeration self-test. The distributable folder is:

```text
dist\WS63_Laser_Host\
├── WS63_Laser_Host.exe
└── internal\
```

Distribute the complete `WS63_Laser_Host` folder. To also create a shortcut for
the locally built copy:

```cmd
build_windows.bat -CreateDesktopShortcut
```

Build requirements can be preinstalled and installation skipped:

```cmd
python -m pip install -r requirements-build.txt
build_windows.bat -SkipInstall
```

Writable application data is stored outside the program directory:

```text
%LOCALAPPDATA%\fbb_ws63\WS63 Laser Host\
├── config\host_ui_config.json
├── logs\
└── generated_images\
```

Set `WS63_LASER_HOST_DATA_DIR` to override this root for tests or portable
development environments. When the updated source tree is run, an existing
source-relative configuration is migrated here automatically; a fresh packaged
copy starts with the built-in defaults until settings are saved.

## Sync to Win11

```bash
./scripts/sync_host_to_win.sh
```

Then run on Win11:

```cmd
cd C:\Users\ZKX\OneDrive\Desktop\ws63_laser_host_ui
python main.py
```
