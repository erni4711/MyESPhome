#!/usr/bin/env python3
"""
Integration test for the web_admin_local ApiTilesHandler.

POSTs tile configuration to the device and GETs it back to verify the round-trip.
Supports both a single-folder {"tiles":[...]} payload and a full Waveshare export
{"grids":{"0":[...],"1":[...]}} — in the latter case all grids are uploaded and
verified unless --folder is specified explicitly.

Usage examples:
  # Upload a specific folder from a Waveshare export:
  python test_web_admin_local_tiles.py --url http://192.168.10.26/admin/tiles \\
      --file waveshare_tiles.json --folder 0

  # Upload ALL folders found in the Waveshare export (default when --folder is omitted):
  python test_web_admin_local_tiles.py --url http://192.168.10.26/admin/tiles \\
      --file waveshare_tiles.json
"""

import sys
import argparse
import json
import os

try:
    import requests
except Exception:
    requests = None
    import urllib.request
    import urllib.error


def load_doc(path):
    with open(path, 'rb') as f:
        raw = f.read()
    try:
        return json.loads(raw.decode('utf-8'))
    except Exception as e:
        print(f"Failed to parse JSON file {path}: {e}", file=sys.stderr)
        raise


def get_folders(doc):
    """Return list of (folder_id_int, tiles_list, payload_str) for every grid in doc."""
    if isinstance(doc, dict) and 'tiles' in doc and isinstance(doc['tiles'], list):
        # Single-folder format: {"tiles": [...]}
        payload = json.dumps(doc, separators=(',', ':'), ensure_ascii=False)
        return [(0, doc['tiles'], payload)]

    if isinstance(doc, dict) and 'grids' in doc and isinstance(doc['grids'], dict):
        result = []
        for key, tiles in sorted(doc['grids'].items(), key=lambda kv: int(kv[0]) if kv[0].isdigit() else 0):
            if isinstance(tiles, list):
                wrapped = {'tiles': tiles}
                payload = json.dumps(wrapped, separators=(',', ':'), ensure_ascii=False)
                folder_id = int(key) if key.isdigit() else 0
                result.append((folder_id, tiles, payload))
        if result:
            return result

    raise ValueError('Unsupported JSON format; must contain top-level "tiles" or "grids"')


def http_post(url, data, headers, timeout=60):
    if requests:
        r = requests.post(url, data=data.encode('utf-8'), headers=headers, timeout=timeout)
        return r.status_code, r.text
    else:
        req = urllib.request.Request(url, data=data.encode('utf-8'), headers=headers, method='POST')
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return resp.getcode(), resp.read().decode('utf-8')
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode('utf-8')


def http_get(url, timeout=60):
    if requests:
        r = requests.get(url, timeout=timeout)
        return r.status_code, r.text
    else:
        req = urllib.request.Request(url, method='GET')
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return resp.getcode(), resp.read().decode('utf-8')
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode('utf-8')


def normalize_tiles(tiles):
    def norm_val(v):
        if isinstance(v, dict):
            return {k: norm_val(v[k]) for k in sorted(v.keys())}
        if isinstance(v, list):
            return [norm_val(x) for x in v]
        return v
    return [norm_val(t) for t in tiles]


def test_folder(base_url, folder_id, expected_tiles, payload):
    """Upload and verify a single folder. Returns True on success."""
    url = base_url.rstrip('/') + f'?folder={folder_id}'
    headers = {'Content-Type': 'application/json'}

    print(f'  POST {url} ({len(expected_tiles)} tiles) ...', end=' ', flush=True)
    status, body = http_post(url, payload, headers)
    if status != 200:
        print(f'FAIL (HTTP {status}): {body}')
        return False
    print(f'OK')

    print(f'  GET  {url} ...', end=' ', flush=True)
    status, body = http_get(url)
    if status != 200:
        print(f'FAIL (HTTP {status}): {body}')
        return False

    try:
        got = json.loads(body)
    except Exception as e:
        print(f'FAIL (bad JSON): {e}')
        return False

    # API returns a bare array []; also accept {"tiles":[...]} for backwards compat
    if isinstance(got, list):
        got_tiles = got
    elif isinstance(got, dict) and isinstance(got.get('tiles'), list):
        got_tiles = got['tiles']
    else:
        print(f'FAIL (response is not a tiles array)')
        return False

    exp_norm = normalize_tiles(expected_tiles)
    got_norm = normalize_tiles(got_tiles)

    if exp_norm == got_norm:
        print(f'OK — {len(got_norm)} tiles match')
        return True
    else:
        print(f'MISMATCH')
        print('  -- expected (first 3) --')
        print('  ' + json.dumps(exp_norm[:3], ensure_ascii=False))
        print('  -- got (first 3) --')
        print('  ' + json.dumps(got_norm[:3], ensure_ascii=False))
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--url', required=True,
                        help='Base URL of tiles endpoint (e.g. http://192.168.10.26/admin/tiles)')
    parser.add_argument('--file', required=True,
                        help='Path to JSON file to upload (Waveshare export or single-folder format)')
    parser.add_argument('--folder', type=int, default=None,
                        help='Folder id to upload (default: all folders found in the file)')
    args = parser.parse_args()

    if not os.path.exists(args.file):
        print(f'File not found: {args.file}', file=sys.stderr)
        return 2

    try:
        doc = load_doc(args.file)
        all_folders = get_folders(doc)
    except Exception as e:
        print(f'Error reading file: {e}', file=sys.stderr)
        return 2

    # Filter to a specific folder when --folder is given
    if args.folder is not None:
        folders = [(fid, tiles, payload) for fid, tiles, payload in all_folders if fid == args.folder]
        if not folders:
            print(f'Folder {args.folder} not found in file. Available: {[f for f,_,_ in all_folders]}',
                  file=sys.stderr)
            return 2
    else:
        folders = all_folders

    print(f'File: {args.file}')
    print(f'Device: {args.url}')
    print(f'Folders to upload: {[f for f,_,_ in folders]}')
    print()

    passed = 0
    failed = 0
    for folder_id, tiles, payload in folders:
        print(f'Folder {folder_id}:')
        ok = test_folder(args.url, folder_id, tiles, payload)
        if ok:
            passed += 1
        else:
            failed += 1
        print()

    total = passed + failed
    print(f'Results: {passed}/{total} folders passed')
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())