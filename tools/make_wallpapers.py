"""Generate 466x466 watch-face wallpapers.

Four styles, rendered at the panel's native size with no text and no bezel:

    aurora_nebula   deep-space clouds and stars
    abstract_glow   soft colour blobs on black
    fluid_flow      warped liquid bands
    layered_bloom   symmetric layered petals

    python tools/make_wallpapers.py                 # write all four
    python tools/make_wallpapers.py --only fluid_flow
    python tools/make_wallpapers.py --out wallpapers

Each is kept deliberately dark around the rim and mid-bright in the centre
so the dial's light ticks, numerals and hands stay readable on top.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

SIZE = 466


def _grid():
    """Normalised coordinates: x,y in [-1,1], plus polar radius and angle."""
    axis = (np.arange(SIZE) - (SIZE - 1) / 2.0) / ((SIZE - 1) / 2.0)
    x, y = np.meshgrid(axis, axis)
    return x, y, np.hypot(x, y), np.arctan2(y, x)


def _blob(x, y, cx, cy, radius):
    """Smooth falloff blob, 1 at the centre and 0 by `radius`."""
    d = np.hypot(x - cx, y - cy) / radius
    return np.clip(1.0 - d * d, 0.0, 1.0) ** 2


def _finish(rgb, radius, *, vignette=0.55, blur=0.0):
    """Apply a rim vignette, clamp, and return an image.

    The vignette matters on a round panel: it stops the wallpaper from
    fighting the outer tick ring.
    """
    fade = np.clip(1.0 - vignette * np.clip(radius - 0.35, 0, None) / 0.65, 0.0, 1.0)
    rgb = rgb * fade[..., None]
    img = Image.fromarray(np.clip(rgb * 255.0, 0, 255).astype(np.uint8), "RGB")
    if blur > 0:
        img = img.filter(ImageFilter.GaussianBlur(blur))
    return img


def aurora_nebula() -> Image.Image:
    x, y, r, _ = _grid()
    rng = np.random.default_rng(7)

    rgb = np.zeros((SIZE, SIZE, 3))
    clouds = [
        (-0.35, -0.30, 0.85, (0.16, 0.85, 0.75)),   # teal
        (0.40, -0.15, 0.80, (0.55, 0.30, 0.95)),    # violet
        (0.05, 0.45, 0.75, (0.95, 0.25, 0.65)),     # magenta
        (-0.55, 0.35, 0.65, (0.20, 0.45, 0.98)),    # blue
        (0.30, 0.30, 0.55, (0.10, 0.70, 0.90)),     # cyan
    ]
    # Warp the sampling grid so the clouds look wind-blown rather than round.
    warp = 0.18 * np.sin(3.0 * y + 1.1) + 0.12 * np.cos(4.0 * x - 0.6)
    for cx, cy, rad, colour in clouds:
        mask = _blob(x + warp, y - warp * 0.7, cx, cy, rad)
        for c in range(3):
            rgb[..., c] += mask * colour[c]

    # Filamentary structure.
    filament = 0.5 + 0.5 * np.sin(7.0 * x + 5.0 * y + 3.0 * warp)
    rgb *= 0.55 + 0.45 * filament[..., None]

    img = _finish(rgb * 0.9, r, vignette=0.5, blur=6)

    # Stars on top so the blur does not smear them away.
    arr = np.asarray(img).astype(np.float32)
    star_mask = rng.random((SIZE, SIZE)) > 0.9985
    brightness = rng.uniform(0.5, 1.0, (SIZE, SIZE))[..., None]
    arr = np.where(star_mask[..., None], np.clip(arr + 255 * brightness, 0, 255), arr)
    return Image.fromarray(arr.astype(np.uint8), "RGB").filter(
        ImageFilter.GaussianBlur(0.4)
    )


def abstract_glow() -> Image.Image:
    x, y, r, _ = _grid()
    rgb = np.zeros((SIZE, SIZE, 3))
    blobs = [
        (-0.45, -0.45, 0.55, (0.98, 0.85, 0.20)),   # yellow
        (0.42, -0.40, 0.60, (0.20, 0.55, 0.98)),    # blue
        (-0.50, 0.40, 0.62, (0.98, 0.20, 0.60)),    # pink
        (0.45, 0.45, 0.58, (0.85, 0.25, 0.85)),     # magenta
        (0.00, 0.05, 0.45, (0.20, 0.90, 0.85)),     # teal centre
        (0.62, 0.05, 0.35, (0.95, 0.35, 0.45)),     # coral
    ]
    for cx, cy, rad, colour in blobs:
        mask = _blob(x, y, cx, cy, rad)
        for c in range(3):
            rgb[..., c] += mask * colour[c]

    # Heavy blur is what turns overlapping discs into the soft glow look.
    return _finish(rgb, r, vignette=0.7, blur=26)


def fluid_flow() -> Image.Image:
    x, y, r, _ = _grid()

    # Iteratively warp the domain: each pass folds the bands over themselves.
    u, v = x * 2.2, y * 2.2
    for _ in range(4):
        u, v = (
            u + 0.55 * np.sin(1.7 * v + 0.9),
            v + 0.55 * np.cos(1.6 * u - 0.4),
        )
    band = 0.5 + 0.5 * np.sin(1.9 * u + 1.3 * v)

    # Orange -> magenta -> violet -> blue ramp.
    stops = np.array(
        [
            [0.99, 0.55, 0.15],
            [0.95, 0.25, 0.45],
            [0.60, 0.25, 0.85],
            [0.20, 0.45, 0.95],
            [0.15, 0.75, 0.85],
        ]
    )
    pos = band * (len(stops) - 1)
    idx = np.clip(pos.astype(int), 0, len(stops) - 2)
    frac = (pos - idx)[..., None]
    rgb = stops[idx] * (1 - frac) + stops[idx + 1] * frac

    # Soft shading so the bands read as volume rather than flat colour.
    shade = 0.65 + 0.35 * np.sin(3.0 * u - 2.0 * v)
    return _finish(rgb * shade[..., None], r, vignette=0.55, blur=3)


def layered_bloom() -> Image.Image:
    x, y, r, theta = _grid()
    rgb = np.zeros((SIZE, SIZE, 3))

    # Three petal rings, each rotated against the one beneath it.
    layers = [
        (8, 0.95, 0.00, (0.10, 0.62, 0.60)),        # teal, outermost
        (8, 0.70, math.pi / 8, (0.85, 0.20, 0.45)), # crimson
        (6, 0.45, math.pi / 6, (0.90, 0.72, 0.30)), # gold, innermost
    ]
    for count, extent, phase, colour in layers:
        # Petal outline in polar form; |cos| gives evenly spaced lobes.
        petal_r = extent * (0.45 + 0.55 * np.abs(np.cos(count * (theta + phase) / 2.0)))
        inside = np.clip(1.0 - (r / np.maximum(petal_r, 1e-3)) ** 2, 0.0, 1.0)
        # Brighten toward each petal's spine for a rounded, layered look.
        spine = 0.55 + 0.45 * np.abs(np.cos(count * (theta + phase) / 2.0))
        mask = inside * spine
        for c in range(3):
            rgb[..., c] = rgb[..., c] * (1 - inside * 0.85) + mask * colour[c]

    return _finish(rgb, r, vignette=0.4, blur=2)


STYLES = {
    "aurora_nebula": aurora_nebula,
    "abstract_glow": abstract_glow,
    "fluid_flow": fluid_flow,
    "layered_bloom": layered_bloom,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="wallpapers", help="output directory")
    parser.add_argument("--only", choices=sorted(STYLES), help="render just one style")
    args = parser.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    names = [args.only] if args.only else list(STYLES)
    for name in names:
        image = STYLES[name]()
        path = out_dir / f"{name}.png"
        image.save(path)
        print(f"{path}  {image.size[0]}x{image.size[1]}")

    print(f"\nUpload one with:\n  python tools/set_wallpaper.py {out_dir}/<name>.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
