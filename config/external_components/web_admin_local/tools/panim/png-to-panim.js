'use strict';

// Converts a PNG sprite sheet into a ".panim" file for the Animation tile.
//
//   node tools/panim/png-to-panim.js <input.png> [options]
//
// Options (key=value):
//   frames=N    number of frames laid out horizontally in the sheet (default 1)
//   fps=N       playback speed in frames/sec (default 8)
//   scale=N     downsample factor: every NxN block in the PNG becomes one
//               native pixel (use this when the art is "blocky" / each logical
//               pixel is drawn as an NxN square). Default 1.
//   out=PATH    output path (default: <input basename>.panim next to the input)
//   bg=#RRGGBB  treat this colour (and fully transparent pixels) as background
//               -> stored black, so it blends with the tile's black background.
//               Default: transparent only.
//
// The sheet width must be frames * frameWidth. Keep the native frame small
// (<=128 px per side); the firmware upscales it crisply on the device.
//
// Requires pngjs:  npm install pngjs

let PNG;
try {
  PNG = require('pngjs').PNG;
} catch (e) {
  console.error('Missing dependency "pngjs". Install it with:\n  npm install pngjs');
  process.exit(1);
}

const fs = require('fs');
const path = require('path');
const { buildPanim, MAX_SIDE } = require('./panim');

function parseArgs(argv) {
  const opts = { frames: 1, fps: 8, scale: 1, out: null, bg: null };
  const positional = [];
  for (const a of argv) {
    const eq = a.indexOf('=');
    if (eq < 0) { positional.push(a); continue; }
    const k = a.slice(0, eq);
    const v = a.slice(eq + 1);
    if (k === 'frames' || k === 'fps' || k === 'scale') opts[k] = parseInt(v, 10);
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

const opts = parseArgs(process.argv.slice(2));
if (!opts.input) {
  console.error('Usage: node png-to-panim.js <input.png> [frames=N] [fps=N] [scale=N] [out=PATH] [bg=#RRGGBB]');
  process.exit(1);
}
if (!opts.frames || opts.frames < 1) opts.frames = 1;
if (!opts.fps || opts.fps < 1) opts.fps = 8;
if (!opts.scale || opts.scale < 1) opts.scale = 1;

const bg = opts.bg ? hexToRgb(opts.bg) : null;
if (opts.bg && !bg) { console.error(`Bad bg colour: ${opts.bg}`); process.exit(1); }

const png = PNG.sync.read(fs.readFileSync(opts.input));
const { width: sheetW, height: sheetH, data } = png;

if (sheetW % opts.frames !== 0) {
  console.error(`Sheet width ${sheetW} is not divisible by frames=${opts.frames}.`);
  process.exit(1);
}
const srcFrameW = sheetW / opts.frames;
const srcFrameH = sheetH;
const frameW = Math.floor(srcFrameW / opts.scale);
const frameH = Math.floor(srcFrameH / opts.scale);

if (frameW < 1 || frameH < 1) { console.error('Frame size collapsed to 0 - check scale.'); process.exit(1); }
if (frameW > MAX_SIDE || frameH > MAX_SIDE) {
  console.error(`Native frame ${frameW}x${frameH} exceeds ${MAX_SIDE}px. Increase scale=.`);
  process.exit(1);
}

function samplePixel(sx, sy) {
  const i = (sy * sheetW + sx) * 4;
  const r = data[i], g = data[i + 1], b = data[i + 2], a = data[i + 3];
  if (a < 128) return [0, 0, 0];
  if (bg && r === bg[0] && g === bg[1] && b === bg[2]) return [0, 0, 0];
  return [r, g, b];
}

const frames = [];
for (let fr = 0; fr < opts.frames; fr++) {
  const baseX = fr * srcFrameW;
  const frame = new Array(frameW * frameH);
  for (let y = 0; y < frameH; y++) {
    for (let x = 0; x < frameW; x++) {
      // Sample the centre of each NxN block (nearest-neighbour downsample).
      const sx = baseX + Math.min(srcFrameW - 1, x * opts.scale + (opts.scale >> 1));
      const sy = Math.min(srcFrameH - 1, y * opts.scale + (opts.scale >> 1));
      frame[y * frameW + x] = samplePixel(sx, sy);
    }
  }
  frames.push(frame);
}

const frameMs = Math.round(1000 / opts.fps);
const buf = buildPanim(frames, frameW, frameH, frameMs);

const outPath = opts.out ||
  path.join(path.dirname(opts.input), path.basename(opts.input, path.extname(opts.input)) + '.panim');
fs.writeFileSync(outPath, buf);
console.log(`wrote ${outPath}: ${frameW}x${frameH}, ${opts.frames} frame(s) @ ${opts.fps}fps (${buf.length} bytes)`);
