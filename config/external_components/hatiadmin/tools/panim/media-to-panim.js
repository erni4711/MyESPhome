'use strict';

// Converts an animated image / video (WebP, GIF, APNG, WebM, MP4, ...) into a
// ".panim" file for the Animation tile. Uses the system ffmpeg/ffprobe to decode
// and downscale -- no npm dependencies.
//
//   node tools/panim/media-to-panim.js <input> [options]
//
// Options (key=value):
//   height=N    native pixel height (default 24). Width is derived from the
//               source aspect ratio. The device upscales this crisply.
//   width=N     native pixel width (optional; overrides aspect-based width).
//   fps=N       playback speed of the resulting animation (default 12).
//   bg=#RRGGBB  treat this colour as background -> stored black (blends with the
//               black tile). Fully transparent pixels always become black.
//   out=PATH    output path (default: <input basename>.panim next to the input).
//
// Example (white-background Mario sprite, ~16 px tall logical art):
//   node tools/panim/media-to-panim.js mario.webp height=16 fps=10 bg=#ffffff out=mario.panim

const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');
const { buildPanim, MAX_SIDE, MAX_FRAMES } = require('./panim');

function parseArgs(argv) {
  const opts = { height: 24, width: 0, fps: 12, bg: null, out: null, input: null };
  const positional = [];
  for (const a of argv) {
    const eq = a.indexOf('=');
    if (eq < 0) { positional.push(a); continue; }
    const k = a.slice(0, eq);
    const v = a.slice(eq + 1);
    if (k === 'height' || k === 'width' || k === 'fps') opts[k] = parseInt(v, 10);
    else if (k === 'out') opts.out = v;
    else if (k === 'bg') opts.bg = v;
  }
  opts.input = positional[0];
  return opts;
}

function hexToRgb(hex) {
  const m = /^#?([0-9a-f]{6})$/i.exec(hex || '');
  if (!m) return null;
  const n = parseInt(m[1], 16);
  return [(n >> 16) & 0xff, (n >> 8) & 0xff, n & 0xff];
}

function probeSize(input) {
  const out = execFileSync('ffprobe', [
    '-v', 'error', '-select_streams', 'v:0',
    '-show_entries', 'stream=width,height',
    '-of', 'json', input,
  ]).toString();
  const j = JSON.parse(out);
  const s = j.streams && j.streams[0];
  if (!s || !s.width || !s.height) throw new Error('ffprobe: no video stream / size');
  return { w: s.width, h: s.height };
}

const opts = parseArgs(process.argv.slice(2));
if (!opts.input) {
  console.error('Usage: node media-to-panim.js <input> [height=N] [width=N] [fps=N] [bg=#RRGGBB] [out=PATH]');
  process.exit(1);
}
if (!fs.existsSync(opts.input)) { console.error(`Input not found: ${opts.input}`); process.exit(1); }
if (!opts.fps || opts.fps < 1) opts.fps = 12;

const bg = opts.bg ? hexToRgb(opts.bg) : null;
if (opts.bg && !bg) { console.error(`Bad bg colour: ${opts.bg}`); process.exit(1); }

// Work out the native target size (keep it small; the device upscales).
const src = probeSize(opts.input);
let H = opts.height && opts.height > 0 ? opts.height : 24;
let W = opts.width && opts.width > 0 ? opts.width : Math.max(1, Math.round(src.w * (H / src.h)));
W = Math.min(W, MAX_SIDE);
H = Math.min(H, MAX_SIDE);
if (W % 2 === 1) W += 1;  // ffmpeg is happiest with even dimensions

// Decode + nearest-neighbour downscale to a raw RGBA stream in a temp file.
const tmp = path.join(os.tmpdir(), `panim_${process.pid}_${Date.now()}.rgba`);
try {
  execFileSync('ffmpeg', [
    '-v', 'error', '-y',
    '-i', opts.input,
    '-vf', `scale=${W}:${H}:flags=neighbor`,
    '-f', 'rawvideo', '-pix_fmt', 'rgba',
    tmp,
  ]);

  const raw = fs.readFileSync(tmp);
  const pxPerFrame = W * H;
  const bytesPerFrame = pxPerFrame * 4;
  let total = Math.floor(raw.length / bytesPerFrame);
  if (total < 1) throw new Error('ffmpeg produced no frames');

  // Pick frame indices (even subsample if there are more than the format allows).
  const indices = [];
  if (total <= MAX_FRAMES) {
    for (let i = 0; i < total; i++) indices.push(i);
  } else {
    for (let i = 0; i < MAX_FRAMES; i++) indices.push(Math.floor((i * total) / MAX_FRAMES));
  }

  const frames = indices.map((fi) => {
    const base = fi * bytesPerFrame;
    const frame = new Array(pxPerFrame);
    for (let p = 0; p < pxPerFrame; p++) {
      const o = base + p * 4;
      const r = raw[o], g = raw[o + 1], b = raw[o + 2], a = raw[o + 3];
      if (a < 128) frame[p] = [0, 0, 0];
      else if (bg && r === bg[0] && g === bg[1] && b === bg[2]) frame[p] = [0, 0, 0];
      else frame[p] = [r, g, b];
    }
    return frame;
  });

  const frameMs = Math.round(1000 / opts.fps);
  const buf = buildPanim(frames, W, H, frameMs);

  const outPath = opts.out ||
    path.join(path.dirname(opts.input), path.basename(opts.input, path.extname(opts.input)) + '.panim');
  fs.writeFileSync(outPath, buf);
  console.log(`wrote ${outPath}: ${W}x${H}, ${frames.length}/${total} frame(s) @ ${opts.fps}fps (${buf.length} bytes)`);
} finally {
  try { fs.unlinkSync(tmp); } catch (e) { /* ignore */ }
}
