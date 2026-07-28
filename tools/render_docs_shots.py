"""Render the README's page mockups.

These are renderings, not photographs: the geometry, colours and fonts are
taken from main.c so a layout change can be re-rendered rather than
re-photographed.

    python tools/render_docs_shots.py                 # every page + the strip
    python tools/render_docs_shots.py --only codex

Output goes to examples/esp-idf/11_CodexPedometer/docs/.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

SIZE = 466
CENTER = SIZE / 2
SS = 2  # supersample the whole panel

REPO = Path(__file__).resolve().parent.parent
OUT_DIR = REPO / "examples/esp-idf/11_CodexPedometer/docs"
ICONS_DIR = REPO / "examples/esp-idf/11_CodexPedometer/main"

# main.c: s_theme_color
THEME = {
    "battery": 0x30D158, "temp": 0xFF9F0A, "weekday": 0xBF5AF2, "date": 0xFF453A,
    "codex": 0xFFD166, "steps": 0x9DFF35, "music": 0x18D7F5, "weather": 0x64D2FF,
}
ARC_BG = 0x17212B
CAPTION = 0x9AA7B5
WHITE = 0xF7FBFF

# main.c: UI_ARC_SIZE / UI_ARC_TOP / UI_ARC_WIDTH and the label offsets
ARC_TOP, ARC_SIZE, ARC_WIDTH = 13, 440, 14
Y_ICON, Y_CAPTION, Y_VALUE = -120, -70, -8
Y_GOAL, Y_STATUS, Y_UPDATED = 52, 86, 172
Y_METRIC_VALUE, Y_METRIC_LABEL, METRIC_COL = 126, 148, 104

CJK = r"C:\Windows\Fonts\Deng.ttf"
CJK_BOLD = r"C:\Windows\Fonts\Dengb.ttf"
LATIN = r"C:\Windows\Fonts\segoeui.ttf"
LATIN_BOLD = r"C:\Windows\Fonts\seguisb.ttf"


def rgb(value: int) -> tuple[int, int, int]:
    return (value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF


def font(path: str, size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(path, size * SS)


class Panel:
    """A 466x466 round panel, drawn supersampled and downscaled at the end."""

    def __init__(self):
        self.image = Image.new("RGB", (SIZE * SS, SIZE * SS), (0, 0, 0))
        self.draw = ImageDraw.Draw(self.image)

    def arc(self, value: float, color: int, *, start=135, sweep=270) -> None:
        box = [ARC_TOP * SS, ARC_TOP * SS,
               (ARC_TOP + ARC_SIZE) * SS, (ARC_TOP + ARC_SIZE) * SS]
        self.draw.arc(box, start, start + sweep, fill=rgb(ARC_BG), width=ARC_WIDTH * SS)
        if value > 0:
            self.draw.arc(box, start, start + sweep * value, fill=rgb(color),
                          width=ARC_WIDTH * SS)

    def text(self, value: str, y: int, size: int, color: int, *, bold=False,
             x: int = 0, cjk: bool | None = None) -> None:
        if cjk is None:
            cjk = any("\u4e00" <= c <= "\u9fff" for c in value)
        path = (CJK_BOLD if bold else CJK) if cjk else (LATIN_BOLD if bold else LATIN)
        self.draw.text(((CENTER + x) * SS, (CENTER + y) * SS), value, fill=rgb(color),
                       font=font(path, size), anchor="mm")

    def icon(self, name: str, y: int, color: int, scale: float = 1.0) -> None:
        alpha = load_icon_alpha(name)
        if scale != 1.0:
            side = int(alpha.width * scale)
            alpha = alpha.resize((side, side), Image.LANCZOS)
        alpha = alpha.resize((alpha.width * SS, alpha.height * SS), Image.LANCZOS)
        tint = Image.new("RGB", alpha.size, rgb(color))
        pos = (int((CENTER * SS) - alpha.width / 2), int(((CENTER + y) * SS) - alpha.height / 2))
        self.image.paste(tint, pos, alpha)

    def status_bar(self, title: str, clock: str = "10:24") -> None:
        """Unused: set_active_page_locked() hides the bar on every page."""
        self.draw.text((160 * SS, 45 * SS), clock, fill=rgb(WHITE),
                       font=font(LATIN, 19), anchor="mm")
        self.draw.line([(224 * SS, 32 * SS), (224 * SS, 62 * SS)], fill=rgb(0x4C5866),
                       width=SS)
        self.draw.text((277 * SS, 45 * SS), title, fill=rgb(WHITE),
                       font=font(CJK if any("\u4e00" <= c <= "\u9fff" for c in title)
                                 else LATIN, 19), anchor="mm")
        self.draw.rounded_rectangle(
            [326 * SS, 38 * SS, 352 * SS, 52 * SS], radius=3 * SS,
            outline=rgb(0x8E8E93), width=max(1, SS))
        self.draw.rectangle([329 * SS, 41 * SS, 345 * SS, 49 * SS], fill=rgb(THEME["battery"]))

    def metrics(self, columns) -> None:
        for i, (label, value, color) in enumerate(columns):
            x = (i - 1) * METRIC_COL
            self.text(value, Y_METRIC_VALUE, 22, color, x=x)
            self.text(label, Y_METRIC_LABEL, 13, CAPTION, x=x)

    def save(self, name: str) -> Path:
        # Mask off the corners the round panel never shows.
        mask = Image.new("L", self.image.size, 0)
        ImageDraw.Draw(mask).ellipse([0, 0, self.image.size[0] - 1, self.image.size[1] - 1],
                                     fill=255)
        out = Image.new("RGB", self.image.size, (0, 0, 0))
        out.paste(self.image, (0, 0), mask)
        out = out.resize((SIZE, SIZE), Image.LANCZOS)
        OUT_DIR.mkdir(parents=True, exist_ok=True)
        path = OUT_DIR / f"{name}.png"
        out.save(path)
        return path


_icon_cache: dict[str, Image.Image] = {}


def load_icon_alpha(name: str) -> Image.Image:
    """Pull an icon's alpha channel straight out of the generated ui_icons.c."""
    if name in _icon_cache:
        return _icon_cache[name]

    source = (ICONS_DIR / "ui_icons.c").read_text(encoding="utf-8")
    marker = f"static const uint8_t {name}_data[] = {{"
    start = source.index(marker) + len(marker)
    end = source.index("};", start)
    values = [int(v, 16) for v in source[start:end].replace("\n", "").split(",") if v.strip()]
    side = int(math.isqrt(len(values) // 4))
    alpha = Image.new("L", (side, side))
    alpha.putdata(values[3::4])  # B, G, R, A
    _icon_cache[name] = alpha
    return alpha


# --------------------------------------------------------------------- pages
def page_clock() -> Panel:
    p = Panel()
    p.draw.ellipse([(CENTER - 228) * SS, (CENTER - 228) * SS,
                    (CENTER + 228) * SS, (CENTER + 228) * SS],
                   outline=rgb(0x1B242E), width=SS)
    # Hour ticks.
    for i in range(60):
        angle = math.radians(i * 6 - 90)
        inner = 228 - (16 if i % 5 == 0 else 7)
        colour = 0xE6EDF5 if i % 5 == 0 else 0x39434F
        p.draw.line([((CENTER + math.cos(angle) * inner) * SS,
                      (CENTER + math.sin(angle) * inner) * SS),
                     ((CENTER + math.cos(angle) * 228) * SS,
                      (CENTER + math.sin(angle) * 228) * SS)],
                    fill=rgb(colour), width=(3 if i % 5 == 0 else 1) * SS)

    # Four corner complications: ring + value + caption.
    corners = [(-104, -74, "电量", "82%", THEME["battery"], 0.82),
               (104, -74, "温度", "27°", THEME["temp"], 0.62),
               (-104, 74, "星期", "二", THEME["weekday"], 3 / 7),
               (104, 74, "日期", "28", THEME["date"], 28 / 31)]
    for cx, cy, caption, value, colour, fraction in corners:
        box = [(CENTER + cx - 40) * SS, (CENTER + cy - 40) * SS,
               (CENTER + cx + 40) * SS, (CENTER + cy + 40) * SS]
        p.draw.arc(box, 135, 45, fill=rgb(ARC_BG), width=6 * SS)
        p.draw.arc(box, 135, 135 + 270 * fraction, fill=rgb(colour), width=6 * SS)
        p.text(value, cy - 8, 17, WHITE, x=cx)
        p.text(caption, cy + 16, 13, colour, x=cx)

    # Hands.
    for length, width, colour, angle_deg in ((92, 7, 0xF7FBFF, -52),
                                             (140, 5, 0xF7FBFF, 118),
                                             (156, 2, 0xFF453A, 210)):
        angle = math.radians(angle_deg - 90)
        p.draw.line([(CENTER * SS, CENTER * SS),
                     ((CENTER + math.cos(angle) * length) * SS,
                      (CENTER + math.sin(angle) * length) * SS)],
                    fill=rgb(colour), width=width * SS)
    p.draw.ellipse([(CENTER - 5) * SS, (CENTER - 5) * SS,
                    (CENTER + 5) * SS, (CENTER + 5) * SS], fill=rgb(0xFF453A))
    p.text("10:24:36", 176, 20, 0xC9D3DC)
    return p


def page_weather() -> Panel:
    p = Panel()
    p.arc(0.62, THEME["weather"])
    # Sun behind a cloud, the way draw_weather_icon paints "partly cloudy".
    sun = (CENTER + 14, CENTER - 140)
    p.draw.ellipse([(sun[0] - 22) * SS, (sun[1] - 22) * SS,
                    (sun[0] + 22) * SS, (sun[1] + 22) * SS], fill=rgb(0xFFD166))
    for cx, cy, r in ((CENTER - 26, CENTER - 122, 20), (CENTER + 2, CENTER - 132, 26),
                      (CENTER + 28, CENTER - 120, 18)):
        p.draw.ellipse([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS],
                       fill=rgb(0xE6EDF5))
    p.draw.rounded_rectangle([(CENTER - 46) * SS, (CENTER - 122) * SS,
                              (CENTER + 46) * SS, (CENTER - 100) * SS],
                             radius=11 * SS, fill=rgb(0xE6EDF5))
    p.text("天气", -50, 22, 0xE6EDF5)
    p.text("27°", 6, 46, WHITE, bold=True)
    p.text("多云", 54, 17, 0xFFD166)
    p.text("青岛", 80, 15, 0x8DDFFF)
    p.text("高德接口", 106, 13, 0x92A0AD)
    p.metrics([("低温", "23°", WHITE), ("更新", "10:20", WHITE), ("高温", "29°", WHITE)])
    return p


def page_codex() -> Panel:
    p = Panel()
    p.arc(0.02, THEME["codex"])
    p.icon("ui_icon_terminal", Y_ICON, 0x30D158)
    p.text("Codex", Y_CAPTION, 22, THEME["codex"])
    p.text("98%", Y_VALUE, 46, WHITE, bold=True)
    p.text("11_CodexPedometer - 3分", 32, 13, 0x8DA0B4)
    p.text("在线", Y_GOAL, 13, 0x9DFF35)
    p.text("重置 08-04 11:52", Y_STATUS, 13, 0x92A0AD)
    p.metrics([("已用", "2%", WHITE), ("状态", "工作", 0x30D158), ("剩余", "98%", WHITE)])
    return p


def page_agent() -> Panel:
    p = Panel()
    p.arc(1.0, 0xFFD60A)
    p.icon("ui_icon_ai", Y_ICON, 0xFFD60A)
    p.text("AI 状态", Y_CAPTION, 22, 0xE6EDF5)
    p.text("等待确认", Y_VALUE - 14, 30, 0xFFD60A)
    p.text("16:49:35", 24, 19, 0xC9D3DC)
    p.text("Claude - 11_CodexPedometer", Y_GOAL, 13, 0x9AA7B5)
    p.text("Bash", Y_STATUS, 13, 0x92A0AD)
    p.metrics([("CODEX", "空闲", WHITE), ("CLAUDE", "等待", WHITE), ("时长", "3秒", WHITE)])
    return p


def page_music() -> Panel:
    p = Panel()
    p.arc(0.46, THEME["music"])
    p.icon("ui_icon_music", -168, THEME["music"])
    p.text("音乐", -118, 22, 0xE6EDF5)

    bands = [0.35, 0.55, 0.8, 0.62, 0.9, 0.7, 0.45, 0.6, 0.85, 0.5, 0.3, 0.42,
             0.66, 0.38, 0.25]
    pitch, width, baseline, max_h = 18, 12, 336, 150
    start_x = (SIZE - (len(bands) * pitch - 5)) / 2
    for i, level in enumerate(bands):
        hue = i / (len(bands) - 1)
        colour = (int(0x18 + hue * 0xE0), int(0xD7 - hue * 0x90), int(0xF5 - hue * 0xC0))
        height = max(4, int(level * max_h))
        x = start_x + i * pitch
        p.draw.rounded_rectangle([x * SS, (baseline - height) * SS,
                                  (x + width) * SS, baseline * SS],
                                 radius=3 * SS, fill=colour)
        peak = baseline - height - 8
        p.draw.rectangle([x * SS, peak * SS, (x + width) * SS, (peak + 3) * SS], fill=colour)
    p.draw.line([(98 * SS, 338 * SS), (368 * SS, 338 * SS)], fill=rgb(0x1A2734), width=2 * SS)
    p.metrics([("低音", "4", WHITE), ("音量 dB", "-56", WHITE), ("高音", "0", WHITE)])
    return p


def page_stock() -> Panel:
    p = Panel()
    p.arc(0.55, 0xFF453A)
    p.text("SZ002241", -150, 19, 0x9AA7B5)
    p.text("歌尔股份", -118, 24, WHITE)
    p.text("24.86", -58, 44, 0xFF453A, bold=True)
    p.text("+0.92  +3.84%", -14, 20, 0xFF453A)

    # Intraday trace, in the 300x96 canvas at y=262.
    points = [0.30, 0.38, 0.30, 0.46, 0.58, 0.52, 0.66, 0.62, 0.78, 0.70, 0.86, 0.82,
              0.90, 0.84, 0.93]
    left, width, top, height = 83, 300, 262, 96
    p.draw.line([(left * SS, (top + height / 2) * SS), ((left + width) * SS,
                 (top + height / 2) * SS)], fill=rgb(0x1E2A38), width=SS)
    step = width / (len(points) - 1)
    p.draw.line([((left + i * step) * SS, (top + height - 8 - v * (height - 16)) * SS)
                 for i, v in enumerate(points)], fill=rgb(0xFF453A), width=3 * SS,
                joint="curve")
    p.text("15:00  ·  轻点看日K", 172, 13, 0x92A0AD)
    p.metrics([("最高", "25.10", WHITE), ("最低", "23.88", WHITE), ("换手", "3.76%", WHITE)])
    return p


def page_settings() -> Panel:
    p = Panel()
    p.text("设置", -181, 22, WHITE)
    p.text("IP  192.168.50.111", -151, 13, 0x92A0AD)
    rows = [("亮度 80%", -125, -101, 0.8), ("音量 45%", -61, -37, 0.45)]
    for label, label_y, slider_y, fraction in rows:
        p.text(label, label_y, 13, CAPTION)
        p.draw.rounded_rectangle([113 * SS, (233 + slider_y - 7) * SS,
                                  353 * SS, (233 + slider_y + 7) * SS],
                                 radius=7 * SS, fill=rgb(0x2A2E33))
        p.draw.rounded_rectangle([113 * SS, (233 + slider_y - 7) * SS,
                                  (113 + 240 * fraction) * SS, (233 + slider_y + 7) * SS],
                                 radius=7 * SS, fill=rgb(THEME["music"]))
    p.text("待机 3 分钟", 3, 13, WHITE)
    p.draw.rounded_rectangle([113 * SS, 274 * SS, 353 * SS, 288 * SS], radius=7 * SS,
                             fill=rgb(0x2A2E33))
    p.text("表盘", 71, 13, CAPTION)
    for i, name in enumerate(("INFO", "PIXEL", "TILES")):
        active = i == 0
        x0 = CENTER + (i - 1) * 84 - 39
        p.draw.rounded_rectangle([x0 * SS, 326 * SS, (x0 + 78) * SS, 360 * SS], radius=17 * SS,
                                 fill=rgb(THEME["music"] if active else 0x1B222B))
        p.text(name, 110, 13, 0x03181F if active else 0xC9D3DC, x=(i - 1) * 84)
    p.text("语言", 135, 13, CAPTION)
    for i, (name, active) in enumerate((("中文", True), ("English", False))):
        x0 = CENTER + (-46 if i == 0 else 46) - 42
        p.draw.rounded_rectangle([x0 * SS, 390 * SS, (x0 + 84) * SS, 422 * SS], radius=16 * SS,
                                 fill=rgb(THEME["music"] if active else 0x1B222B))
        p.text(name, 173, 13, 0x03181F if active else 0xC9D3DC, x=(-46 if i == 0 else 46))
    return p


PAGES = {
    "clock": page_clock,
    "weather": page_weather,
    "codex": page_codex,
    "agent": page_agent,
    "music": page_music,
    "stock": page_stock,
    "settings": page_settings,
}


def strip(names, out_name: str, per_row: int = 4) -> Path:
    gap = 18
    rows = (len(names) + per_row - 1) // per_row
    width = per_row * SIZE + (per_row + 1) * gap
    height = rows * SIZE + (rows + 1) * gap
    sheet = Image.new("RGB", (width, height), (5, 7, 10))
    for i, name in enumerate(names):
        tile = Image.open(OUT_DIR / f"{name}.png")
        x = gap + (i % per_row) * (SIZE + gap)
        y = gap + (i // per_row) * (SIZE + gap)
        sheet.paste(tile, (x, y))
    path = OUT_DIR / f"{out_name}.png"
    sheet.save(path)
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", choices=sorted(PAGES), help="render one page")
    args = parser.parse_args()

    names = [args.only] if args.only else list(PAGES)
    for name in names:
        path = PAGES[name]().save(name)
        print(f"{path.relative_to(REPO)}")
    if not args.only:
        print(strip(["clock", "weather", "codex", "agent"], "overview").relative_to(REPO))
        print(strip(["music", "stock", "settings"], "overview2", per_row=3).relative_to(REPO))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
