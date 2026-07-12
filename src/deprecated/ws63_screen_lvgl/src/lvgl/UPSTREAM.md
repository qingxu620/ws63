# LVGL Upstream Information

- **Project**: LVGL (Light and Versatile Graphics Library)
- **Version**: v9.3.0
- **License**: MIT
- **Repository**: https://github.com/lvgl/lvgl.git
- **Commit**: c033a98afddd65aaafeebea625382a94020fe4a7
- **Tag**: v9.3.0

## Imported Files

The following files were imported from the upstream LVGL v9.3.0 release
to supplement the existing library tree:

- `src/core/*.c` (13 source files)
- `src/core/*.h` (21 header files)
- `LICENCE.txt` (MIT license text)

## Import Reason

The `src/core/` subdirectory was omitted from the initial project setup.
Two `.gitignore` patterns contributed to this gap:

1. `src/.gitignore` contained a bare `core` pattern (now changed to `/core`)
   which matched any file or directory named `core` at any depth.
2. Root `.gitignore` contained `**/core` which matched any directory
   named `core` under the entire repository tree.

These patterns were intended to exclude Unix core dump files (e.g., `core`,
`core.12345`), but also inadvertently excluded the LVGL core source directory.

## Status

The LVGL core files now exist on disk. Both `.gitignore` files have been
updated with negation rules for the LVGL `core/` path.

After a fresh `git clone`, these files will be present if they are tracked
by git, or can be restored by running:

```bash
git checkout -- src/deprecated/ws63_screen_lvgl/src/lvgl/src/core/
```
