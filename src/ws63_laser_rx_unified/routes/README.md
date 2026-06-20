# RX Integrated Routes

R1 only creates the route directory layout. No real route implementation is
compiled here yet.

Planned routes:

- `legacy_uart/`: reuse the mature `ws63_laser_single` LaserGRBL stream path.
- `legacy_wifi/`: reuse the mature `ws63_laser_wifi` SoftAP TCP stream path.
- `sle_job/`: reuse the mature `ws63_laser_sle_job/receiver` packet/cache path.

Do not put shared Grbl parsing logic here. Each route keeps its own proven
protocol parser until a later phase explicitly proves a safe refactor.
