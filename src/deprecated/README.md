# Deprecated WS63 Laser Experiments

These projects are kept for reference or hardware diagnostics. They are not the
main product firmware line.

- `ws63_laser_wireless`: old TX Grbl proxy / wireless motion-command experiment.
- `ws63_sle_laser`: early SLE passthrough experiment.
- `ws63_laser_sle_bridge`: SLE transparent bridge experiment.
- `ws63_laser_sle_job`: retired standalone SLE TX/RX job experiment. The active
  TX board firmware now lives in `src/ws63_laser_sle_tx`; the product RX is
  `src/ws63_laser_rx_unified`.
- `ws63_laser_host_ui`: PySide6 host UI skeleton kept for reference; current SLE work uses `src/ws63_laser_host_ui`.
- `ws63_laser_sle_job_host`: previous SLE host-side debug app, superseded by `src/ws63_laser_host_ui`.
- `ws63_sd_card_test`: standalone MSP3223 SD-card diagnostic firmware. Build it
  only with `scripts/build_sd_card_test_firmware.sh` when debugging TF card
  hardware or formatting issues.
- `ws63_screen_st7796_ft6336`: historical ST7796/FT6336 screen self-test project. Current selected screen reference is `MSP3223/`; LVGL may temporarily reuse the old driver files until the MSP3223/ILI9341V port is complete.
- `ws63_screen_lvgl`: previous LVGL minimal port, superseded by `src/ws63_screen_panel_lvgl`.

Current active projects:

- `src/ws63_laser_sle_tx`: TX board, UART-to-SLE structured job sender.
- `src/ws63_laser_rx_unified`: product RX firmware.
- `src/ws63_laser_host_ui`: Win11 host-side app.
- `src/ws63_screen_panel_lvgl_refactor`: MSP3223 screen panel firmware.
