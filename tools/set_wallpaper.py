"""Send a watch-face wallpaper to the ESP32 over Wi-Fi.

Takes any image, centre-crops it to a square, resizes to the panel's
466x466, converts to raw RGB565 and POSTs it. The device stores the frame
in its own flash partition and draws the dial ticks on top of it.

    python tools/set_wallpaper.py photo.jpg
    python tools/set_wallpaper.py photo.jpg --host 192.168.50.111
    python tools/set_wallpaper.py --clear

Conversion happens here rather than on the watch on purpose: decoding a
JPEG at this size needs a large working buffer, and a failure there would
land at boot. A raw frame is a straight memcpy into the dial canvas.
"""

from __future__ import annotations

import argparse
import sys
import urllib.request
from pathlib import Path

WIDTH = 466
HEIGHT = 466
DEFAULT_HOST = "192.168.50.111"
DIM_DEFAULT = 55


def to_rgb565(image, dim_percent: int) -> bytes:
    """Pack to little-endian RGB565, optionally darkened.

    The dial's ticks, numerals and hands are light, so a full-brightness
    photo leaves them unreadable; dimming keeps the wallpaper legible as a
    background rather than competing with the time.
    """
    from PIL import Image, ImageEnhance

    image = image.convert("RGB")

    # Centre-crop to a square before resizing so nothing is stretched.
    w, h = image.size
    side = min(w, h)
    image = image.crop(
        ((w - side) // 2, (h - side) // 2, (w - side) // 2 + side, (h - side) // 2 + side)
    )
    image = image.resize((WIDTH, HEIGHT), Image.LANCZOS)

    if dim_percent > 0:
        image = ImageEnhance.Brightness(image).enhance(1.0 - dim_percent / 100.0)

    out = bytearray(WIDTH * HEIGHT * 2)
    pixels = image.load()
    i = 0
    for y in range(HEIGHT):
        for x in range(WIDTH):
            r, g, b = pixels[x, y]
            value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out[i] = value & 0xFF
            out[i + 1] = (value >> 8) & 0xFF
            i += 2
    return bytes(out)


def post(url: str, payload: bytes | None) -> str:
    request = urllib.request.Request(
        url,
        data=payload,
        method="POST" if payload is not None else "GET",
        headers={"Content-Type": "application/octet-stream"} if payload else {},
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read().decode("utf-8", "replace")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", nargs="?", help="any image file")
    parser.add_argument("--host", default=DEFAULT_HOST, help="watch IP or 192.168.4.1 on its AP")
    parser.add_argument(
        "--dim",
        type=int,
        default=DIM_DEFAULT,
        help=f"darken by this %% so the dial stays readable (default {DIM_DEFAULT})",
    )
    parser.add_argument("--clear", action="store_true", help="remove the stored wallpaper")
    parser.add_argument("--save", metavar="FILE", help="also write the raw frame locally")
    args = parser.parse_args()

    if args.clear:
        post(f"http://{args.host}/wallpaper/clear", None)
        print("Wallpaper cleared.")
        return 0

    if not args.image:
        parser.error("give an image, or --clear")

    try:
        from PIL import Image
    except ImportError:
        print("Pillow is required:  python -m pip install Pillow", file=sys.stderr)
        return 1

    path = Path(args.image)
    if not path.exists():
        print(f"No such file: {path}", file=sys.stderr)
        return 1

    with Image.open(path) as image:
        raw = to_rgb565(image, args.dim)
    print(f"{path.name}: {len(raw)} bytes ({WIDTH}x{HEIGHT} RGB565, dimmed {args.dim}%)")

    if args.save:
        Path(args.save).write_bytes(raw)
        print(f"Saved raw frame to {args.save}")

    try:
        reply = post(f"http://{args.host}/wallpaper", raw)
    except Exception as exc:  # noqa: BLE001 - report any transport failure plainly
        print(f"Upload to {args.host} failed: {exc}", file=sys.stderr)
        return 1

    print(f"Device replied: {reply.strip()}")
    print("Switch to the INFO face to see it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
