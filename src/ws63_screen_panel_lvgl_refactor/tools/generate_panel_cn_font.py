#!/usr/bin/env python3
"""Generate the WS63 panel Chinese LVGL font from local UI strings.

Uses lv_font_conv to render a sparse, uncompressed 14 px, 4 bpp LVGL font.
Requires: Node.js / npx, and a Chinese TTF font on the system.

On Windows, the script auto-extracts SimSun TTF from simsun.ttc using fonttools
if a standalone TTF is not available.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PANEL_ROOT = ROOT
FONT_OUTPUT = PANEL_ROOT / "src" / "ui" / "fonts" / "lv_font_panel_cn_14.c"
FONT_NAME = "lv_font_panel_cn_14"
FONT_SIZE = 14
FONT_BPP = 4

# Candidate font paths (Windows + Linux)
_FONT_CANDIDATES = [
    # Windows simsun.ttc needs extraction; we handle that below
    r"C:\Windows\Fonts\simsun.ttc",
    # WSL access to Windows fonts. Prefer standalone TTF to avoid TTC extraction.
    "/mnt/c/Windows/Fonts/simhei.ttf",
    "/mnt/c/Windows/Fonts/simsun.ttc",
    "/mnt/c/Windows/Fonts/msyh.ttc",
    # Linux
    "/usr/share/fonts/truetype/droid/DroidSansFallback.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/wqy-microhei/wqy-microhei.ttc",
]

TOOLS_DIR = PANEL_ROOT / "tools"


def _find_system_font() -> str:
    """Return a usable TTF/TTC path, extracting from TTC if needed."""
    for candidate in _FONT_CANDIDATES:
        if os.path.isfile(candidate):
            if candidate.endswith(".ttc"):
                # Need to extract TTF from TTC
                extracted = TOOLS_DIR / "_simsun_extracted.ttf"
                if extracted.exists():
                    return str(extracted)
                try:
                    from fontTools.ttLib import TTCollection

                    ttc = TTCollection(candidate)
                    ttc[0].save(str(extracted))
                    print(f"Extracted TTF from {candidate} -> {extracted}")
                    return str(extracted)
                except ImportError:
                    print(
                        "fonttools not installed. Run: pip install fonttools",
                        file=sys.stderr,
                    )
                    sys.exit(1)
                except Exception as e:
                    print(f"Failed to extract from {candidate}: {e}", file=sys.stderr)
                    continue
            else:
                return candidate
    print(
        "No Chinese TTF font found. Install fonts-noto-cjk or wqy-microhei.",
        file=sys.stderr,
    )
    sys.exit(1)


def collect_chars() -> list[str]:
    """Scan source files for Chinese characters used in UI strings."""
    chars: set[str] = set()
    for path in (PANEL_ROOT / "src").rglob("*.[ch]"):
        text = path.read_text(errors="ignore")
        chars.update(
            ch
            for ch in text
            if "\u4e00" <= ch <= "\u9fff" or ch in "，。！？：；（）"
        )

    # Also include chars from the existing font file (preserves any extras)
    if FONT_OUTPUT.exists():
        old = FONT_OUTPUT.read_text(errors="ignore")
        for match in re.finditer(r"U\+([0-9A-Fa-f]{4,6})", old):
            chars.add(chr(int(match.group(1), 16)))

    return sorted(chars, key=ord)


def build_range_arg(chars: list[str]) -> str:
    """Build the -r range argument for lv_font_conv."""
    ranges: list[str] = []
    i = 0
    while i < len(chars):
        start = ord(chars[i])
        end = start
        while i + 1 < len(chars) and ord(chars[i + 1]) == end + 1:
            i += 1
            end = ord(chars[i])
        if start == end:
            ranges.append(f"0x{start:X}")
        else:
            ranges.append(f"0x{start:X}-0x{end:X}")
        i += 1
    return ",".join(ranges)


def post_process(output_path: Path) -> None:
    """Fix include path and add fallback font declaration."""
    text = output_path.read_text(encoding="utf-8")

    # Fix include: lv_font_conv outputs conditional includes; we want simple
    text = text.replace(
        '#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n#include "lvgl.h"\n#else\n#include "lvgl/lvgl.h"\n#endif',
        '#include "lvgl.h"',
    )

    # Ensure fallback is set (lv_font_conv sets NULL by default)
    if ".fallback = NULL," in text:
        text = text.replace(".fallback = NULL,", ".fallback = &lv_font_montserrat_14,")

    # Ensure extern declaration exists before the font struct
    if "extern const lv_font_t lv_font_montserrat_14;" not in text:
        text = text.replace(
            "/*Initialize a public general font descriptor*/\n#if LVGL_VERSION_MAJOR >= 8",
            "/*Initialize a public general font descriptor*/\nextern const lv_font_t lv_font_montserrat_14;\n\n#if LVGL_VERSION_MAJOR >= 8",
        )

    output_path.write_text(text, encoding="utf-8")


def generate() -> None:
    chars = collect_chars()
    if not chars:
        raise RuntimeError("no Chinese glyphs found in source files")

    font_path = _find_system_font()
    range_arg = build_range_arg(chars)

    output_tmp = FONT_OUTPUT.with_name(FONT_OUTPUT.stem + "_new.c")

    cmd = [
        "lv_font_conv",
        "--font",
        font_path,
        "-r",
        range_arg,
        "--size",
        str(FONT_SIZE),
        "--bpp",
        str(FONT_BPP),
        "--format",
        "lvgl",
        "--no-compress",
        "--no-prefilter",
        "-o",
        str(output_tmp),
        "--lv-font-name",
        FONT_NAME,
    ]

    # Add npm global bin to PATH so lv_font_conv can be found
    env = os.environ.copy()
    npm_global_bin = Path(os.popen("npm prefix -g").read().strip()) if shutil.which("lv_font_conv") is None else None
    if npm_global_bin and npm_global_bin.exists():
        env["PATH"] = str(npm_global_bin) + os.pathsep + env.get("PATH", "")

    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if result.returncode != 0:
        print(f"lv_font_conv failed:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)

    post_process(output_tmp)

    # Replace old file
    if FONT_OUTPUT.exists():
        FONT_OUTPUT.unlink()
    output_tmp.rename(FONT_OUTPUT)

    # Cleanup extracted font
    extracted = TOOLS_DIR / "_simsun_extracted.ttf"
    if extracted.exists():
        extracted.unlink()

    print(f"Generated {FONT_OUTPUT}")
    print(f"Glyphs: {len(chars)}")
    print("Chars: " + "".join(chars))


if __name__ == "__main__":
    generate()
