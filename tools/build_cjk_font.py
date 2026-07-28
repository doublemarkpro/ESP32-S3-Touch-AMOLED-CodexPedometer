"""Regenerate the watch's CJK font with full common-Chinese coverage.

The bundled lv_font_source_han_sans_sc_16_cjk.c is an upstream LVGL sample
subset aimed at Japanese; it is missing many everyday Chinese characters
(云, 阴, 雷, 雾, 东, 风 ...), which is why several screens fall back to
English. This builds a replacement covering GB2312 level 1 (6763 characters,
both tiers - level 2 carries name characters like 鑫) plus ASCII and the FontAwesome glyphs
LVGL's built-in symbols use.

    python tools/build_cjk_font.py            # 16 px, into the component
    python tools/build_cjk_font.py --size 20  # a larger cut

Needs Node and lv_font_conv:  npm install -g lv_font_conv
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

# LVGL's LV_SYMBOL_* set - keep these or every icon on the watch turns into
# a missing-glyph box.
FA_SYMBOLS = [
    61441, 61448, 61451, 61452, 61453, 61457, 61459, 61461, 61465, 61468,
    61473, 61478, 61479, 61480, 61502, 61507, 61512, 61515, 61516, 61517,
    61521, 61522, 61523, 61524, 61543, 61544, 61550, 61552, 61553, 61556,
    61559, 61560, 61561, 61563, 61587, 61589, 61636, 61637, 61639, 61641,
    61664, 61671, 61674, 61683, 61724, 61732, 61787, 61931, 62016, 62017,
    62018, 62019, 62020, 62087, 62099, 62212, 62189, 62810, 63426, 63650,
]

# Punctuation and symbols the UI actually prints.
EXTRA_CODEPOINTS = [
    0x00B0,  # degree
    0x00B7,  # middle dot
    0x2013, 0x2014,  # dashes
    0x2018, 0x2019, 0x201C, 0x201D,  # quotes
    0x2026,  # ellipsis
    0x3001, 0x3002,  # 、。
    0xFF01, 0xFF08, 0xFF09, 0xFF0C, 0xFF1A, 0xFF1B, 0xFF1F,  # ！（），：；？
    0x2191, 0x2193,  # arrows for gain/loss
]


def gb2312_hanzi() -> list[int]:
    """Enumerate both GB2312 tiers: 6763 simplified characters.

    Level 1 (rows 0xB0-0xD7, cells 0xA1-0xFE, row 0xD7 stopping at 0xF9) is
    the 3755 most common characters. Level 2 (rows 0xD8-0xF7) carries the
    rarer ones - surnames and company names live there (鑫, 昇, 淼 ...), so
    a stock or city the user types would otherwise render as boxes.
    """
    points: list[int] = []
    for row in range(0xB0, 0xF8):
        last = 0xF9 if row == 0xD7 else 0xFE
        for cell in range(0xA1, last + 1):
            try:
                char = bytes([row, cell]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            points.append(ord(char))
    return points


def compress_ranges(points: list[int]) -> str:
    """Collapse a sorted code point list into lv_font_conv range syntax."""
    points = sorted(set(points))
    parts: list[str] = []
    start = prev = points[0]
    for point in points[1:]:
        if point == prev + 1:
            prev = point
            continue
        parts.append(f"0x{start:X}" if start == prev else f"0x{start:X}-0x{prev:X}")
        start = prev = point
    parts.append(f"0x{start:X}" if start == prev else f"0x{start:X}-0x{prev:X}")
    return ",".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size", type=int, default=16, help="pixel height")
    parser.add_argument("--bpp", type=int, default=4, choices=[1, 2, 4, 8])
    parser.add_argument(
        "--font",
        default=r"C:\Windows\Fonts\Deng.ttf",
        help="source TTF/OTF (Dengxian: clean at small sizes)",
    )
    parser.add_argument(
        "--symbol-font",
        default=None,
        help="FontAwesome file for LV_SYMBOL_*; skipped when absent",
    )
    parser.add_argument("--out", default=None, help="output .c path")
    args = parser.parse_args()

    font = Path(args.font)
    if not font.exists():
        print(f"Source font not found: {font}", file=sys.stderr)
        return 1

    repo = Path(__file__).resolve().parent.parent
    # Kept in main/fonts/ - a managed_components path would be wiped by any
    # component refresh and is gitignored, so the font would silently revert.
    out = Path(args.out) if args.out else (
        repo / "examples/esp-idf/11_CodexPedometer/main/fonts/app_font_cjk_16.c"
    )

    ranges = compress_ranges(
        list(range(0x20, 0x80)) + EXTRA_CODEPOINTS + gb2312_hanzi()
    )

    print(f"Building {args.size}px font -> {out.name}")
    print(f"  glyphs: ASCII + {len(EXTRA_CODEPOINTS)} symbols + GB2312 level 1+2")

    # 3755 ranges blow past the Windows command-line limit, so drive
    # lv_font_conv through Node instead of argv.
    args_json = [
        "--no-compress", "--no-prefilter",
        "--bpp", str(args.bpp),
        "--size", str(args.size),
        "--font", str(font).replace("\\", "/"),
        "-r", ranges,
        "--format", "lvgl",
        "-o", str(out).replace("\\", "/"),
        "--force-fast-kern-format",
    ]
    runner = Path(__file__).parent / "_font_runner.js"
    runner.write_text(
        "const {execFileSync} = require('child_process');\n"
        "const args = " + repr(args_json).replace("'", '"') + ";\n"
        "const path = require('path');\n"
        "const conv = path.join(process.env.APPDATA, 'npm', 'node_modules',\n"
        "  'lv_font_conv', 'lv_font_conv.js');\n"
        "process.argv = [process.argv[0], conv, ...args];\n"
        "require(conv);\n",
        encoding="utf-8",
    )
    result = subprocess.run(["node", str(runner)], shell=sys.platform == "win32")
    runner.unlink(missing_ok=True)
    if result.returncode != 0:
        print("lv_font_conv failed", file=sys.stderr)
        return result.returncode

    # lv_font_conv derives its symbol from the output filename, so the name
    # follows whatever the caller asked for - two sizes must not collide.
    symbol = out.stem
    text = out.read_text(encoding="utf-8")
    text = text.replace('#include "../../lvgl.h"', '#include "lvgl.h"')
    out.write_text(text, encoding="utf-8")

    size_kb = out.stat().st_size / 1024
    print(f"Wrote {out} ({size_kb:.0f} KB of C source)")
    print(f"Symbol: {symbol}  (declare with LV_FONT_DECLARE in main.c)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
