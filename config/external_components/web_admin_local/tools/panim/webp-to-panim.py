#!/usr/bin/env python3
"""Convert an animated WebP/GIF (or any multi-frame image Pillow can read) into a
.panim file for the Animation tile.

Usage:
  python webp-to-panim.py <input> [--out PATH] [--fps N] [--max N] [--bg #RRGGBB]

- Frames are read with Pillow, downscaled with nearest-neighbour (crisp pixels).
- Fully transparent pixels -> black. With --bg, that colour also -> black, so it
  blends with the black Animation tile.
- If the source is already small (<= --max per side) it is kept 1:1 so pixel art
  stays pixel-perfect; the device upscales it on screen.

.panim format (PAN2, with alpha): 'PAN2' + u16 w + u16 h + u16 frames +
u16 frame_ms (little endian), then frames*w*h pixels as 4 bytes R,G,B,A.
Background / transparent pixels are written with A=0 so they show the tile
behind them on the device.
"""
import argparse
import struct
import sys

from PIL import Image, ImageSequence

MAX_SIDE = 128
MAX_FRAMES = 64


def parse_hex(s):
    s = s.lstrip("#")
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("--out")
    ap.add_argument("--fps", type=int, default=10)
    ap.add_argument("--max", type=int, default=48,
                    help="native size cap per side; source kept 1:1 if it fits")
    ap.add_argument("--native", type=int, default=0,
                    help="force native longest side to N px (center-sampled); use "
                         "this to recover crisp pixel art from a soft/large source")
    ap.add_argument("--bg", help="background colour to make black, e.g. #ffffff")
    args = ap.parse_args()

    bg = parse_hex(args.bg) if args.bg else None

    im = Image.open(args.input)
    frames = [f.convert("RGBA") for f in ImageSequence.Iterator(im)]
    if not frames:
        sys.exit("no frames decoded")

    sw, sh = frames[0].size
    # Target native size.
    if args.native > 0:
        # Force longest side to --native (keep aspect).
        if sw >= sh:
            tw = args.native
            th = max(1, round(sh * args.native / sw))
        else:
            th = args.native
            tw = max(1, round(sw * args.native / sh))
    elif max(sw, sh) <= args.max:
        tw, th = sw, sh  # already small -> keep 1:1
    else:
        if sw >= sh:
            tw, th = args.max, max(1, round(sh * args.max / sw))
        else:
            th, tw = args.max, max(1, round(sw * args.max / sh))
    tw = min(tw, MAX_SIDE)
    th = min(th, MAX_SIDE)

    # Even subsample if there are more frames than the format allows.
    if len(frames) > MAX_FRAMES:
        idx = [round(i * len(frames) / MAX_FRAMES) for i in range(MAX_FRAMES)]
        frames = [frames[i] for i in idx]

    body = bytearray()
    for fr in frames:
        px = fr.load()
        fw, fh = fr.size
        for y in range(th):
            # Sample the centre of each target cell -> clean nearest-neighbour
            # down/upscale that lands in the middle of each source block.
            sy = min(fh - 1, int((y + 0.5) * fh / th))
            for x in range(tw):
                sx = min(fw - 1, int((x + 0.5) * fw / tw))
                r, g, b, a = px[sx, sy]
                if a < 128 or (bg and (r, g, b) == bg):
                    body += b"\x00\x00\x00\x00"  # transparent
                else:
                    body += bytes((r, g, b, 255))

    frame_ms = max(20, min(2000, round(1000 / max(1, args.fps))))
    header = b"PAN2" + struct.pack("<HHHH", tw, th, len(frames), frame_ms)

    out = args.out or (args.input.rsplit(".", 1)[0] + ".panim")
    with open(out, "wb") as f:
        f.write(header + bytes(body))
    print(f"wrote {out}: {tw}x{th}, {len(frames)} frame(s) @ {args.fps}fps "
          f"({len(header) + len(body)} bytes)")


if __name__ == "__main__":
    main()
