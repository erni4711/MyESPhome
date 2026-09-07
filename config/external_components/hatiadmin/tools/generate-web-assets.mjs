#!/usr/bin/env node

import { createHash } from 'node:crypto';
import { gzipSync } from 'node:zlib';
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const checkOnly = process.argv.includes('--check');

const assets = [
  {
    key: 'Css',
    source: 'assets/admin.css',
    generated: 'generated/admin_css_gzip.inc',
    extension: 'css',
    contentType: 'text/css; charset=utf-8'
  },
  {
    key: 'Js',
    source: 'assets/admin.js',
    generated: 'generated/admin_js_gzip.inc',
    extension: 'js',
    contentType: 'application/javascript; charset=utf-8'
  }
];

function normalizedSource(relativePath) {
  return readFileSync(resolve(repoRoot, relativePath), 'utf8')
    .replace(/\r\n?/g, '\n');
}

function gzipDeterministic(data) {
  const compressed = gzipSync(data, { level: 9, mtime: 0 });
  // zlib may use a platform-specific gzip OS marker. It has no semantic
  // meaning, so normalize it to "unknown" for byte-identical generation.
  compressed[9] = 255;
  return compressed;
}

function byteInclude(data) {
  const lines = [];
  for (let offset = 0; offset < data.length; offset += 12) {
    const bytes = Array.from(data.subarray(offset, offset + 12), value =>
      `0x${value.toString(16).padStart(2, '0')}`);
    lines.push(`  ${bytes.join(', ')},`);
  }
  return `${lines.join('\n')}\n`;
}

function writeOrCheck(relativePath, expected) {
  const absolutePath = resolve(repoRoot, relativePath);
  if (checkOnly) {
    let current = '';
    try {
      current = readFileSync(absolutePath, 'utf8').replace(/\r\n?/g, '\n');
    } catch {
      throw new Error(`Generated file is missing: ${relativePath}`);
    }
    if (current !== expected) {
      throw new Error(
        `Generated file is stale: ${relativePath}\n` +
        'Run: node tools/generate-web-assets.mjs');
    }
    return;
  }
  writeFileSync(absolutePath, expected, 'utf8');
}

const generated = assets.map(asset => {
  const source = normalizedSource(asset.source);
  const sourceBytes = Buffer.from(source, 'utf8');
  const hash = createHash('sha256').update(sourceBytes).digest('hex');
  const gzip = gzipDeterministic(sourceBytes);
  const gzipHash = createHash('sha256').update(gzip).digest('hex');
  writeOrCheck(asset.generated, byteInclude(gzip));
  return { ...asset, hash, gzipHash, sourceBytes, gzip };
});

const metaLines = [
  '#ifndef WEB_ADMIN_ASSETS_META_H',
  '#define WEB_ADMIN_ASSETS_META_H',
  '',
  '#include <stddef.h>',
  '',
  'namespace web_admin_assets_generated {',
  ''
];

for (const asset of generated) {
  const shortHash = asset.hash.slice(0, 12);
  metaLines.push(
    `inline constexpr char kAdmin${asset.key}Path[] =`,
    `    "/admin/assets/admin.${shortHash}.${asset.extension}";`,
    `inline constexpr char kAdmin${asset.key}Etag[] =`,
    `    "\\\"${asset.gzipHash}\\\"";`,
    `inline constexpr char kAdmin${asset.key}ContentType[] =`,
    `    "${asset.contentType}";`,
    `inline constexpr size_t kAdmin${asset.key}SourceSize = ${asset.sourceBytes.length};`,
    `inline constexpr size_t kAdmin${asset.key}GzipSize = ${asset.gzip.length};`,
    ''
  );
}

metaLines.push(
  '}  // namespace web_admin_assets_generated',
  '',
  '#endif  // WEB_ADMIN_ASSETS_META_H',
  ''
);
writeOrCheck(
  'generated/web_admin_assets.h',
  metaLines.join('\n'));

// Also update the inline metadata block in web_admin_assets.cpp so the build
// works even when the generated/ header is not in the compiler's include path.
const inlineBlock = [
  '// BEGIN_GENERATED_META — managed by tools/generate-web-assets.mjs, do not edit',
  '// Wrapped in #ifndef so it is skipped when generated/web_admin_assets.h is included.',
  '#ifndef WEB_ADMIN_ASSETS_META_H',
  'namespace web_admin_assets_generated {',
  ''
];
for (const asset of generated) {
  const shortHash = asset.hash.slice(0, 12);
  inlineBlock.push(
    `inline constexpr char kAdmin${asset.key}Path[] =`,
    `    "/admin/assets/admin.${shortHash}.${asset.extension}";`,
    `inline constexpr char kAdmin${asset.key}Etag[] =`,
    `    "\\\"${asset.gzipHash}\\\"";`,
    `inline constexpr char kAdmin${asset.key}ContentType[] =`,
    `    "${asset.contentType}";`,
    `inline constexpr size_t kAdmin${asset.key}SourceSize = ${asset.sourceBytes.length};`,
    `inline constexpr size_t kAdmin${asset.key}GzipSize = ${asset.gzip.length};`,
    ''
  );
}
inlineBlock.push(
  '}  // namespace web_admin_assets_generated',
  '#endif  // WEB_ADMIN_ASSETS_META_H',
  '// END_GENERATED_META'
);
const newBlock = inlineBlock.join('\n');

const cppPath = resolve(repoRoot, 'web_admin_assets.cpp');
const cppSrc = readFileSync(cppPath, 'utf8').replace(/\r\n?/g, '\n');
const beginTag = '// BEGIN_GENERATED_META';
const endTag   = '// END_GENERATED_META';
const start = cppSrc.indexOf(beginTag);
const end   = cppSrc.indexOf(endTag);
if (start !== -1 && end !== -1) {
  const updated = cppSrc.slice(0, start) + newBlock + cppSrc.slice(end + endTag.length);
  if (checkOnly) {
    if (cppSrc.replace(/\r\n?/g, '\n') !== updated) {
      throw new Error('web_admin_assets.cpp metadata is stale. Run: node tools/generate-web-assets.mjs');
    }
  } else {
    writeFileSync(cppPath, updated, 'utf8');
  }
}

const mode = checkOnly ? 'verified' : 'generated';
for (const asset of generated) {
  console.log(
    `${asset.source}: ${asset.sourceBytes.length} -> ${asset.gzip.length} bytes (${mode})`);
}
