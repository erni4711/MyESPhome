'use strict';

// Generates a couple of ORIGINAL demo animations so there is something to drop
// onto the SD card and select right away. No third-party / copyrighted sprite
// art is bundled -- to animate your own characters, use png-to-panim.js with
// art you have the rights to.
//
//   node tools/panim/make-demos.js
//
// Writes <repo>/sd-card/animations/*.panim. Copy that folder's contents to
// /animations on the SD card.

const fs = require('fs');
const path = require('path');
const { buildPanim } = require('./panim');

const W = 16;
const H = 16;

const COL = {
  bg: [0, 0, 0],
  pot: [196, 98, 54],
  potDark: [150, 70, 38],
  stem: [54, 150, 58],
  leaf: [78, 185, 80],
  center: [245, 210, 70],
  petal: [230, 80, 90],
  ball: [250, 205, 70],
  ballHi: [255, 240, 170],
  shadow: [30, 30, 30],
};

function newFrame() {
  const f = new Array(W * H);
  for (let i = 0; i < f.length; i++) f[i] = COL.bg;
  return f;
}
function set(f, x, y, c) {
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  f[y * W + x] = c;
}
function rect(f, x0, y0, x1, y1, c) {
  for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) set(f, x, y, c);
}

// --- Growing plant: 6 frames --------------------------------------------------

function plantFrame(stemLen, flowerStage, withLeaves) {
  const f = newFrame();
  // Pot
  rect(f, 4, 11, 11, 11, COL.potDark); // rim
  rect(f, 5, 12, 10, 14, COL.pot);
  rect(f, 6, 15, 9, 15, COL.potDark);
  // Stem
  const topY = 11 - stemLen;
  rect(f, 7, topY, 8, 10, COL.stem);
  // Leaves
  if (withLeaves) {
    rect(f, 5, 11 - 3, 6, 11 - 3, COL.leaf);
    set(f, 6, 11 - 4, COL.leaf);
    rect(f, 9, 11 - 5, 10, 11 - 5, COL.leaf);
    set(f, 9, 11 - 6, COL.leaf);
  }
  // Flower at the stem top
  if (flowerStage >= 1) {
    rect(f, 7, topY, 8, topY + 1, COL.center); // bud / center
  }
  if (flowerStage >= 2) {
    set(f, 6, topY, COL.petal);
    set(f, 9, topY + 1, COL.petal);
    set(f, 7, topY - 1, COL.petal);
  }
  if (flowerStage >= 3) {
    rect(f, 7, topY - 1, 8, topY - 1, COL.petal); // top
    rect(f, 7, topY + 2, 8, topY + 2, COL.petal); // bottom
    rect(f, 5, topY, 5, topY + 1, COL.petal);     // left
    rect(f, 10, topY, 10, topY + 1, COL.petal);   // right
  }
  return f;
}

function makePlant() {
  const frames = [
    plantFrame(2, 0, false),
    plantFrame(4, 0, false),
    plantFrame(6, 0, true),
    plantFrame(8, 1, true),
    plantFrame(8, 2, true),
    plantFrame(8, 3, true),
  ];
  return buildPanim(frames, W, H, 380);
}

// --- Bouncing ball: 8 frames --------------------------------------------------

function ballFrame(cy, squash) {
  const f = newFrame();
  rect(f, 5, 15, 10, 15, COL.shadow); // ground shadow
  const cx = 7; // 7..8 wide ball
  const rx = squash ? 3 : 2;
  const ry = squash ? 1 : 2;
  for (let y = -ry; y <= ry; y++) {
    for (let x = -rx; x <= rx; x++) {
      if ((x * x) / (rx * rx + 0.01) + (y * y) / (ry * ry + 0.01) <= 1.05) {
        set(f, cx + x, cy + y, COL.ball);
      }
    }
  }
  set(f, cx - 1, cy - 1, COL.ballHi); // highlight
  return f;
}

function makeBall() {
  const ys = [4, 6, 8, 10, 12, 10, 8, 6];
  const frames = ys.map((cy, i) => ballFrame(cy, cy >= 12));
  return buildPanim(frames, W, H, 90);
}

// --- Write --------------------------------------------------------------------

const outDir = path.resolve(__dirname, '..', '..', 'sd-card', 'animations');
fs.mkdirSync(outDir, { recursive: true });

const outputs = [
  ['plant.panim', makePlant()],
  ['ball.panim', makeBall()],
];

for (const [name, buf] of outputs) {
  const p = path.join(outDir, name);
  fs.writeFileSync(p, buf);
  console.log(`wrote ${p} (${buf.length} bytes)`);
}
console.log(`\nCopy the contents of ${outDir} to /animations on the SD card.`);
