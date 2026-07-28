"""Render the music-page note icon and append it to ui_icons.c.

Geometry follows Lucide's `music-4` (ISC licensed, same family as the icons
already in ui_icons.c): two note heads, two stems and a double beam. The heads
are filled rather than stroked - at 56 px an outlined head reads as a donut.

    python tools/build_music_icon.py

Writes ui_icon_music into main/ui_icons.c and declares it in ui_icons.h.
Re-running replaces the previous block instead of stacking copies.
"""

from __future__ import annotations

import math
import re
from pathlib import Path

from PIL import Image, ImageDraw

NAME = "ui_icon_music"
SIZE = 56
SS = 8  # supersampling factor
VIEWBOX = 24.0

REPO = Path(__file__).resolve().parent.parent
ICONS_C = REPO / "examples/esp-idf/11_CodexPedometer/main/ui_icons.c"
ICONS_H = REPO / "examples/esp-idf/11_CodexPedometer/main/ui_icons.h"


def scaled(x: float, y: float) -> tuple[float, float]:
    k = SIZE * SS / VIEWBOX
    return x * k, y * k


def stroke(draw: ImageDraw.ImageDraw, a, b, width: float) -> None:
    """Line with round caps - PIL's joint handling does not cap endpoints."""
    ax, ay = scaled(*a)
    bx, by = scaled(*b)
    draw.line([(ax, ay), (bx, by)], fill=255, width=int(round(width)))
    r = width / 2.0
    for x, y in ((ax, ay), (bx, by)):
        draw.ellipse([x - r, y - r, x + r, y + r], fill=255)


def render() -> Image.Image:
    canvas = Image.new("L", (SIZE * SS, SIZE * SS), 0)
    draw = ImageDraw.Draw(canvas)
    width = 2.0 * SIZE * SS / VIEWBOX  # Lucide's stroke-width: 2

    # Stems: down the left at x=9, down the right at x=21.
    stroke(draw, (9, 18), (9, 5), width)
    stroke(draw, (21, 3), (21, 16), width)
    # Beams: the top one closes the stems, the second sits 4 units below.
    stroke(draw, (9, 5), (21, 3), width)
    stroke(draw, (9, 9), (21, 7), width)

    # Filled heads, tilted the way a written note head is.
    for cx, cy in ((6, 18), (18, 16)):
        head = Image.new("L", (SIZE * SS, SIZE * SS), 0)
        hd = ImageDraw.Draw(head)
        px, py = scaled(cx, cy)
        rx, ry = scaled(3.6, 0)[0], scaled(2.9, 0)[0]
        hd.ellipse([px - rx, py - ry, px + rx, py + ry], fill=255)
        head = head.rotate(20, resample=Image.BICUBIC, center=(px, py))
        canvas = Image.composite(Image.new("L", canvas.size, 255), canvas, head)
        draw = ImageDraw.Draw(canvas)

    return canvas.resize((SIZE, SIZE), Image.LANCZOS)


def to_c_bytes(alpha: Image.Image) -> str:
    """ARGB8888 as LVGL stores it in memory: B, G, R, A."""
    rows = []
    data = alpha.load()
    values: list[str] = []
    for y in range(SIZE):
        for x in range(SIZE):
            a = data[x, y]
            values.extend((f"0x{0xFF:02x}",) * 3 + (f"0x{a:02x}",))
    for i in range(0, len(values), 16):
        rows.append("    " + ", ".join(values[i:i + 16]) + ",")
    return "\n".join(rows)


def main() -> int:
    icon = render()
    block = (
        f"\nstatic const uint8_t {NAME}_data[] = {{\n"
        f"{to_c_bytes(icon)}\n"
        f"}};\n\n"
        f"const lv_image_dsc_t {NAME} = {{\n"
        f"    .header = {{\n"
        f"        .magic = LV_IMAGE_HEADER_MAGIC,\n"
        f"        .cf = LV_COLOR_FORMAT_ARGB8888,\n"
        f"        .flags = 0,\n"
        f"        .w = {SIZE},\n"
        f"        .h = {SIZE},\n"
        f"        .stride = {SIZE * 4},\n"
        f"        .reserved_2 = 0,\n"
        f"    }},\n"
        f"    .data_size = sizeof({NAME}_data),\n"
        f"    .data = {NAME}_data,\n"
        f"    .reserved = NULL,\n"
        f"    .reserved_2 = NULL,\n"
        f"}};\n"
    )

    source = ICONS_C.read_text(encoding="utf-8")
    pattern = re.compile(
        rf"\nstatic const uint8_t {NAME}_data\[\].*?^}};\n",
        re.DOTALL | re.MULTILINE,
    )
    source = pattern.sub("", source)
    ICONS_C.write_text(source.rstrip("\n") + "\n" + block, encoding="utf-8")

    header = ICONS_H.read_text(encoding="utf-8")
    decl = f"extern const lv_image_dsc_t {NAME};"
    if decl not in header:
        ICONS_H.write_text(header.rstrip("\n") + "\n" + decl + "\n", encoding="utf-8")

    print(f"Wrote {NAME} ({SIZE}x{SIZE}) into {ICONS_C.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
