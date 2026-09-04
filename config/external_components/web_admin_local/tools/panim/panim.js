'use strict';

// Shared encoder for the ".panim" pixel-animation format used by the firmware's
// "Animation" tile type (src/types/pixelanim/renderer.cpp).
//
// File layout (little-endian header, then raw frames):
//   0  4  magic 'P','A','N','1'
//   4  2  width        (uint16 LE)
//   6  2  height       (uint16 LE)
//   8  2  frame_count  (uint16 LE)
//   10 2  frame_ms     (uint16 LE)  per-frame duration in ms
//   12 .. frame_count * width*height*2 bytes
//          Each pixel is RGB565 written big-endian. On the little-endian ESP32
//          this lands in memory as the byte-swapped value the display expects
//          (LV_COLOR_FORMAT_RGB565_SWAPPED) -- the same convention the JPEG
//          icon path already uses.

const HEADER_BYTES = 12;
const MAX_SIDE = 128;
const MAX_FRAMES = 64;

function rgb565(r, g, b) {
  return (((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3)) & 0xffff;
}

// frames: array of frames; each frame is a flat array of length width*height,
// where every entry is an [r, g, b] triple (0..255). Row-major, top-left first.
function buildPanim(frames, width, height, frameMs) {
  if (!Array.isArray(frames) || frames.length === 0) {
    throw new Error('buildPanim: need at least one frame');
  }
  if (width < 1 || width > MAX_SIDE || height < 1 || height > MAX_SIDE) {
    throw new Error(`buildPanim: width/height must be 1..${MAX_SIDE}`);
  }
  if (frames.length > MAX_FRAMES) {
    throw new Error(`buildPanim: at most ${MAX_FRAMES} frames`);
  }
  const px = width * height;

  const header = Buffer.alloc(HEADER_BYTES);
  header.write('PAN1', 0, 'ascii');
  header.writeUInt16LE(width, 4);
  header.writeUInt16LE(height, 6);
  header.writeUInt16LE(frames.length, 8);
  header.writeUInt16LE(Math.max(20, Math.min(2000, frameMs | 0)), 10);

  const body = Buffer.alloc(px * 2 * frames.length);
  let off = 0;
  for (const frame of frames) {
    if (frame.length !== px) {
      throw new Error(`buildPanim: frame has ${frame.length} px, expected ${px}`);
    }
    for (let i = 0; i < px; i++) {
      const c = frame[i];
      body.writeUInt16BE(rgb565(c[0] | 0, c[1] | 0, c[2] | 0), off);
      off += 2;
    }
  }
  return Buffer.concat([header, body]);
}

module.exports = { buildPanim, rgb565, HEADER_BYTES, MAX_SIDE, MAX_FRAMES };
