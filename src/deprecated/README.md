# Deprecated WS63 Laser Experiments

These projects are kept for reference only and are no longer exposed in `ws63-liteos-app menuconfig`.

- `ws63_laser_wireless`: old TX Grbl proxy / wireless motion-command experiment.
- `ws63_sle_laser`: early SLE passthrough experiment.
- `ws63_laser_sle_bridge`: SLE transparent bridge experiment.
- `ws63_laser_host_ui`: PySide6 host UI skeleton kept for reference; current SLE work uses `src/ws63_laser_host_ui`.
- `ws63_laser_sle_job_host`: previous SLE host-side debug app, superseded by `src/ws63_laser_host_ui`.
- `ws63_screen_st7796_ft6336`: historical ST7796/FT6336 screen self-test project. Current selected screen reference is `MSP3223/`; LVGL may temporarily reuse the old driver files until the MSP3223/ILI9341V port is complete.
- `ws63_screen_lvgl`: previous LVGL minimal port, superseded by `src/ws63_screen_panel_lvgl`.

Current active laser projects:

- `ws63_laser_single`: wired stable baseline.
- `ws63_laser_wifi`: WiFi SoftAP TCP Grbl endpoint.
- `ws63_laser_sle_job`: SLE TX/RX structured job path.
- `ws63_laser_host_ui`: current SLE host-side app (mainline).
- `ws63_screen_panel_lvgl`: current LVGL panel UI (mainline).
- `ws63_laser_rx_unified`: route-based integrated RX line.
