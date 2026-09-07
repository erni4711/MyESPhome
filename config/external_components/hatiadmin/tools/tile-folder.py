#!/usr/bin/env python3
"""Download or upload configurable tile folders.

Examples:

    python tile-folder.py download \
        --url http://192.168.10.26/admin/tiles \
        --folder 1 \
        --file folder-1.json

    python tile-folder.py upload \
        --url http://192.168.10.26/admin/tiles \
        --folder 1 \
        --file folder-1.json

    python tile-folder.py download \
        --url http://192.168.10.26/admin/tiles \
        --folder all \
        --file tiles-backup.json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit, urlunsplit
from urllib.request import Request, urlopen


MAX_FILE_BYTES = 128 * 1024


def api_url(base_url: str, folder: int) -> str:
    """Add the folder query while preserving any existing URL path."""
    parsed = urlsplit(base_url)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise ValueError("URL must include an http:// or https:// scheme and host")
    query = urlencode({"folder": folder})
    return urlunsplit((parsed.scheme, parsed.netloc, parsed.path, query, ""))


def tile_array(value: object, description: str) -> list[dict[str, object]]:
    """Extract and validate one tile array."""
    if isinstance(value, list):
        tiles = value
    elif isinstance(value, dict) and isinstance(value.get("tiles"), list):
        tiles = value["tiles"]
    else:
        raise ValueError(f"{description} must be a JSON array or contain 'tiles'")
    if not all(isinstance(tile, dict) for tile in tiles):
        raise ValueError(f"Every tile in {description} must be a JSON object")
    return tiles


def read_json_file(path: Path, folder: str) -> bytes:
    """Read and validate a single-folder or multi-folder tile JSON file."""
    size = path.stat().st_size
    if folder != "all" and size > MAX_FILE_BYTES:
        raise ValueError(f"Tile file is too large ({size} bytes; limit is {MAX_FILE_BYTES})")
    payload = path.read_bytes()
    try:
        decoded = json.loads(payload)
    except json.JSONDecodeError as exc:
        raise ValueError(f"Tile file is not valid JSON: {exc}") from exc
    if folder == "all":
        if not isinstance(decoded, dict) or not isinstance(decoded.get("grids"), dict):
            raise ValueError("An --folder all upload must contain a 'grids' object")
        for folder_id, grid in decoded["grids"].items():
            if str(folder_id) not in {str(i) for i in range(10)}:
                raise ValueError(f"Invalid folder ID in grids: {folder_id!r}")
            encoded = json.dumps(tile_array(grid, f"grid {folder_id}")).encode()
            if len(encoded) > MAX_FILE_BYTES:
                raise ValueError(
                    f"Grid {folder_id} is too large ({len(encoded)} bytes; "
                    f"limit is {MAX_FILE_BYTES})"
                )
    else:
        tile_array(decoded, "tile file")
    return payload


def request(url: str, method: str, body: bytes | None = None) -> bytes:
    headers = {"Accept": "application/json"}
    if body is not None:
        headers["Content-Type"] = "application/json; charset=utf-8"
    req = Request(url, data=body, headers=headers, method=method)
    try:
        with urlopen(req, timeout=30) as response:
            result = response.read()
            if response.status < 200 or response.status >= 300:
                raise RuntimeError(
                    f"Device returned HTTP {response.status}: "
                    f"{result.decode('utf-8', errors='replace')}"
                )
            return result
    except HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace").strip()
        suffix = f": {detail}" if detail else ""
        raise RuntimeError(f"Device returned HTTP {exc.code}{suffix}") from exc


def download(args: argparse.Namespace) -> None:
    if args.folder == "all":
        grids: dict[str, object] = {}
        for folder_id in range(10):
            result = request(api_url(args.url, folder_id), "GET")
            try:
                decoded = json.loads(result)
            except json.JSONDecodeError as exc:
                raise RuntimeError(
                    f"Device returned invalid JSON for folder {folder_id}: {exc}"
                ) from exc
            grids[str(folder_id)] = tile_array(decoded, f"folder {folder_id}")
        decoded = {"grids": grids}
        description = f"all folders ({sum(len(grid) for grid in grids.values())} tiles)"
    else:
        result = request(api_url(args.url, int(args.folder)), "GET")
        try:
            decoded = json.loads(result)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"Device returned invalid JSON: {exc}") from exc
        tiles = tile_array(decoded, f"folder {args.folder}")
        decoded = tiles
        description = f"folder {args.folder} ({len(tiles)} tiles)"
    args.file.parent.mkdir(parents=True, exist_ok=True)
    args.file.write_text(
        json.dumps(decoded, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"Downloaded {description} to {args.file}")


def upload(args: argparse.Namespace) -> None:
    payload = read_json_file(args.file, args.folder)
    decoded = json.loads(payload)
    if args.folder == "all":
        grids = decoded["grids"]
        for folder_id, grid in sorted(grids.items(), key=lambda item: int(item[0])):
            grid_payload = json.dumps(
                {"tiles": tile_array(grid, f"grid {folder_id}")}
            ).encode()
            request(api_url(args.url, int(folder_id)), "POST", grid_payload)
            print(f"Uploaded folder {folder_id} ({len(grid)} tiles)")
    else:
        tiles = tile_array(json.loads(payload), f"folder {args.folder}")
        request(
            api_url(args.url, int(args.folder)),
            "POST",
            json.dumps({"tiles": tiles}).encode(),
        )
        print(f"Uploaded folder {args.folder} from {args.file}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    for command in ("download", "upload"):
        subparser = subparsers.add_parser(command)
        subparser.add_argument(
            "--url",
            required=True,
            help="Tile API URL, for example http://192.168.10.26/admin/tiles",
        )
        subparser.add_argument(
            "--folder",
            required=True,
            help="Folder number (0-9), or 'all' for a complete backup",
        )
        subparser.add_argument("--file", type=Path, required=True, help="JSON file path")
        subparser.set_defaults(handler=download if command == "download" else upload)

    args = parser.parse_args()
    if args.folder != "all":
        try:
            folder_id = int(args.folder)
        except ValueError:
            parser.error("--folder must be a number from 0 to 9 or 'all'")
        if not 0 <= folder_id <= 9:
            parser.error("--folder must be a number from 0 to 9 or 'all'")
    return args


def main() -> int:
    args = parse_args()
    try:
        args.handler(args)
    except (OSError, ValueError, RuntimeError, HTTPError, URLError) as exc:
        print(f"Tile folder operation failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
