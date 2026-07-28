"""Render the page headline icons and append them to ui_icons.c.

Geometry follows Lucide (ISC licensed, the same family as the icons already in
ui_icons.c), drawn from the 24x24 viewBox with stroke-width 2 and round caps:

    ui_icon_music     music-4       music page
    ui_icon_ai        bot           AI status page
    ui_icon_terminal  square-terminal  Codex usage page

    python tools/build_ui_icons_extra.py            # all three
    python tools/build_ui_icons_extra.py --only ai

Re-running replaces the previous block for an icon instead of stacking copies.
The artwork is white so each page can recolor it to its own accent.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

from PIL import Image, ImageDraw

SS = 8  # supersampling factor
VIEWBOX = 24.0
STROKE = 2.0  # Lucide's stroke-width

REPO = Path(__file__).resolve().parent.parent
ICONS_C = REPO / "examples/esp-idf/11_CodexPedometer/main/ui_icons.c"
ICONS_H = REPO / "examples/esp-idf/11_CodexPedometer/main/ui_icons.h"


class Pen:
    """Draws Lucide primitives into a supersampled alpha canvas."""

    def __init__(self, size: int):
        self.size = size
        self.image = Image.new("L", (size * SS, size * SS), 0)
        self.draw = ImageDraw.Draw(self.image)
        self.k = size * SS / VIEWBOX
        self.width = STROKE * self.k

    def _p(self, x: float, y: float) -> tuple[float, float]:
        return x * self.k, y * self.k

    def line(self, a, b) -> None:
        ax, ay = self._p(*a)
        bx, by = self._p(*b)
        self.draw.line([(ax, ay), (bx, by)], fill=255, width=int(round(self.width)))
        r = self.width / 2.0
        for x, y in ((ax, ay), (bx, by)):
            self.draw.ellipse([x - r, y - r, x + r, y + r], fill=255)

    def polyline(self, points) -> None:
        for a, b in zip(points, points[1:]):
            self.line(a, b)

    def rounded_rect(self, x, y, w, h, radius) -> None:
        x0, y0 = self._p(x, y)
        x1, y1 = self._p(x + w, y + h)
        self.draw.rounded_rectangle(
            [x0, y0, x1, y1], radius=radius * self.k, outline=255,
            width=int(round(self.width)),
        )

    def dot(self, cx, cy, radius, *, fill=True, tilt=0.0, squash=1.0) -> None:
        px, py = self._p(cx, cy)
        rx = radius * self.k
        ry = rx * squash
        if fill and not tilt:
            self.draw.ellipse([px - rx, py - ry, px + rx, py + ry], fill=255)
            return
        if fill:
            layer = Image.new("L", self.image.size, 0)
            ImageDraw.Draw(layer).ellipse([px - rx, py - ry, px + rx, py + ry], fill=255)
            layer = layer.rotate(tilt, resample=Image.BICUBIC, center=(px, py))
            self.image = Image.composite(
                Image.new("L", self.image.size, 255), self.image, layer)
            self.draw = ImageDraw.Draw(self.image)
            return
        self.draw.ellipse([px - rx, py - ry, px + rx, py + ry], outline=255,
                          width=int(round(self.width)))

    def result(self) -> Image.Image:
        return self.image.resize((self.size, self.size), Image.LANCZOS)


def icon_music(size: int) -> Image.Image:
    """Lucide music-4, with filled heads: an outlined head reads as a donut."""
    pen = Pen(size)
    pen.line((9, 18), (9, 5))
    pen.line((21, 3), (21, 16))
    pen.line((9, 5), (21, 3))
    pen.line((9, 9), (21, 7))
    for cx, cy in ((6, 18), (18, 16)):
        pen.dot(cx, cy, 3.6, tilt=20, squash=0.8)
    return pen.result()


def icon_ai(size: int) -> Image.Image:
    """Lucide bot: antenna, head, side arms, two eyes."""
    pen = Pen(size)
    pen.line((12, 8), (12, 4.5))          # antenna
    pen.dot(12, 3.4, 1.5, fill=False)     # antenna tip
    pen.rounded_rect(4, 8, 16, 12, 2)     # head
    pen.line((2, 14), (3.4, 14))          # left arm
    pen.line((20.6, 14), (22, 14))        # right arm
    pen.line((9, 13), (9, 15))            # eyes
    pen.line((15, 13), (15, 15))
    return pen.result()


def icon_terminal(size: int) -> Image.Image:
    """Lucide square-terminal: a prompt caret and a command line in a frame."""
    pen = Pen(size)
    pen.rounded_rect(3, 3, 18, 18, 2)
    pen.polyline([(7.5, 9), (11, 12.5), (7.5, 16)])
    pen.line((13, 16), (17, 16))
    return pen.result()


ICONS = {
    "music": ("ui_icon_music", 56, icon_music),
    "ai": ("ui_icon_ai", 54, icon_ai),
    "terminal": ("ui_icon_terminal", 54, icon_terminal),
}


def to_c_bytes(alpha: Image.Image, size: int) -> str:
    """ARGB8888 the way LVGL stores it in memory: B, G, R, A."""
    data = alpha.load()
    values: list[str] = []
    for y in range(size):
        for x in range(size):
            values.extend(("0xff", "0xff", "0xff", f"0x{data[x, y]:02x}"))
    return "\n".join(
        "    " + ", ".join(values[i:i + 16]) + "," for i in range(0, len(values), 16)
    )


def emit(name: str, size: int, image: Image.Image) -> None:
    block = (
        f"\nstatic const uint8_t {name}_data[] = {{\n"
        f"{to_c_bytes(image, size)}\n"
        f"}};\n\n"
        f"const lv_image_dsc_t {name} = {{\n"
        f"    .header = {{\n"
        f"        .magic = LV_IMAGE_HEADER_MAGIC,\n"
        f"        .cf = LV_COLOR_FORMAT_ARGB8888,\n"
        f"        .flags = 0,\n"
        f"        .w = {size},\n"
        f"        .h = {size},\n"
        f"        .stride = {size * 4},\n"
        f"        .reserved_2 = 0,\n"
        f"    }},\n"
        f"    .data_size = sizeof({name}_data),\n"
        f"    .data = {name}_data,\n"
        f"    .reserved = NULL,\n"
        f"    .reserved_2 = NULL,\n"
        f"}};\n"
    )

    source = ICONS_C.read_text(encoding="utf-8")
    # Drop the data array *and* its descriptor: removing only the array leaves
    # a stale dsc pointing at symbols that no longer exist.
    source = re.sub(
        rf"\nstatic const uint8_t {name}_data\[\].*?^}};\n", "", source,
        flags=re.DOTALL | re.MULTILINE,
    )
    source = re.sub(
        rf"\nconst lv_image_dsc_t {name} = \{{.*?^}};\n", "", source,
        flags=re.DOTALL | re.MULTILINE,
    )
    ICONS_C.write_text(source.rstrip("\n") + "\n" + block, encoding="utf-8")

    header = ICONS_H.read_text(encoding="utf-8")
    decl = f"extern const lv_image_dsc_t {name};"
    if decl not in header:
        ICONS_H.write_text(header.rstrip("\n") + "\n" + decl + "\n", encoding="utf-8")
    print(f"Wrote {name} ({size}x{size})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", choices=sorted(ICONS), help="render just one icon")
    parser.add_argument("--preview", action="store_true", help="print an ASCII preview")
    args = parser.parse_args()

    for key in [args.only] if args.only else list(ICONS):
        name, size, render = ICONS[key]
        image = render(size)
        emit(name, size, image)
        if args.preview:
            small = image.resize((30, 30)).load()
            for y in range(30):
                print("".join("#" if small[x, y] > 160 else
                              ("+" if small[x, y] > 70 else ".") for x in range(30)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
