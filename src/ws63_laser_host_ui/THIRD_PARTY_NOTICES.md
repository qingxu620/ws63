# Third-Party Notices

## LaserGRBL Vectorization Workflow Reference

The Host image conversion workspace includes a `LaserGRBL-style vector` mode.
Its data flow is intentionally aligned with the public LaserGRBL vectorization
workflow:

```text
image -> brightness/contrast cleanup -> threshold bitmap -> spot removal
      -> path smoothing -> G-code outline export
```

Reference project:

- Project: LaserGRBL
- Repository: https://github.com/arkypita/LaserGRBL
- Website: https://lasergrbl.com/
- License: GPLv3
- Relevant source area reviewed for this implementation:
  `LaserGRBL/CsPotrace/` and `LaserGRBL/GrblFile.LoadImagePotrace`

LaserGRBL uses Potrace/CsPotrace for bitmap-to-vector tracing. The current WS63
Host implementation does not vendor LaserGRBL source files or CsPotrace source
files. It implements a small deterministic Python pipeline around the existing
WS63 contour tracer and keeps output compatible with the RX job protocol.

If a future version directly ports or bundles LaserGRBL/CsPotrace code, keep the
corresponding GPL notices and license files with the copied source.

