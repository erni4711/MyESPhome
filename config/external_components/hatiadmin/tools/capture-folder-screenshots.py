#!/usr/bin/env python3
"""Capture one screenshot for every folder configured on a HATi device.

The device must expose the HATi admin endpoints and the screenshot component:

    python capture-folder-screenshots.py \
        --device http://192.168.10.26 \
        --home-assistant http://homeassistant.local:8123 \
        --token <home-assistant-token> \
        --device-name living-room

Screenshots are written to ``images/<device-name>/<folder-name>.png``.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit, urlunsplit
from urllib.request import Request, urlopen


DEFAULT_SETTLE_SECONDS = 5.0
DEFAULT_TIMEOUT = 30.0
SECRET_KEYS = (
    "ha_long_lived_access_token",
    "home_assistant_long_lived_token",
    "ha_long_lived_token",
    "HA_TOKEN",
)
FOLDER_ENTITY_PATTERN = re.compile(r"^select\.[^ ]*(folder|page)[^ ]*$", re.IGNORECASE)
INVALID_FILENAME_CHARS = re.compile(r'[<>:"/\\|?*\x00-\x1f]')


def base_url(value: str, description: str) -> str:
    parsed = urlsplit(value)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise ValueError(f"{description} must include an http:// or https:// scheme and host")
    return urlunsplit((parsed.scheme, parsed.netloc, parsed.path.rstrip("/"), "", ""))


def read_token(secrets_path: Path) -> str:
    """Read a Home Assistant token from a simple ESPHome secrets file."""
    try:
        lines = secrets_path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise RuntimeError(f"Could not read secrets file {secrets_path}: {exc}") from exc

    values: dict[str, str] = {}
    for line in lines:
        match = re.match(
            r"^\s*([A-Za-z_][A-Za-z0-9_-]*)\s*:\s*(.*?)\s*(?:#.*)?$", line
        )
        if not match:
            continue
        key, value = match.groups()
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
            value = value[1:-1]
        values[key] = value

    for key in SECRET_KEYS:
        token = values.get(key, "").strip()
        if token:
            return token
    raise RuntimeError(
        f"No Home Assistant token found in {secrets_path}; "
        f"expected one of: {', '.join(SECRET_KEYS)}"
    )


def request(
    url: str,
    method: str = "GET",
    *,
    token: str | None = None,
    body: bytes | None = None,
    accept: str = "application/json",
    timeout: float = DEFAULT_TIMEOUT,
    allowed_errors: set[int] | None = None,
) -> tuple[int, bytes]:
    headers = {"Accept": accept}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if body is not None:
        headers["Content-Type"] = "application/json"
    try:
        with urlopen(
            Request(url, data=body, headers=headers, method=method), timeout=timeout
        ) as response:
            return response.status, response.read()
    except HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace").strip()
        if allowed_errors and exc.code in allowed_errors:
            return exc.code, detail.encode("utf-8")
        suffix = f": {detail}" if detail else ""
        raise RuntimeError(f"{method} {url} returned HTTP {exc.code}{suffix}") from exc
    except URLError as exc:
        raise RuntimeError(f"{method} {url} failed: {exc.reason}") from exc


def json_request(
    url: str, method: str = "GET", *, token: str | None = None, body: object = None
) -> Any:
    encoded = None if body is None else json.dumps(body).encode("utf-8")
    status, response = request(url, method, token=token, body=encoded)
    if not 200 <= status < 300:
        raise RuntimeError(f"{method} {url} returned HTTP {status}")
    try:
        return json.loads(response)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"{method} {url} returned invalid JSON: {exc}") from exc


def fetch_folders(device: str, timeout: float) -> list[dict[str, Any]]:
    data = json_request(f"{device}/admin/folders")
    folders = data if isinstance(data, list) else data.get("folders") if isinstance(data, dict) else None
    if not isinstance(folders, list):
        raise RuntimeError("The device returned no folder list from /admin/folders")

    result = []
    for folder in folders:
        if not isinstance(folder, dict):
            continue
        folder_id = folder.get("id")
        name = str(folder.get("name") or f"Folder {folder_id}").strip()
        if isinstance(folder_id, int) and folder_id >= 0:
            result.append({"id": folder_id, "name": name})
    if not result:
        raise RuntimeError("The device returned an empty folder list")
    return result


def discover_folder_entity(home_assistant: str, token: str, explicit: str | None) -> str:
    states = json_request(f"{home_assistant}/api/states", token=token)
    if not isinstance(states, list):
        raise RuntimeError("Home Assistant returned an invalid state list")

    if explicit and "." in explicit:
        return explicit

    if explicit:
        named_candidates = [
            state.get("entity_id")
            for state in states
            if isinstance(state, dict)
            and isinstance(state.get("entity_id"), str)
            and isinstance(state.get("attributes"), dict)
            and state["attributes"].get("friendly_name") == explicit
        ]
        if len(named_candidates) == 1:
            return named_candidates[0]
        if not named_candidates:
            raise RuntimeError(
                f"Home Assistant has no entity with friendly name {explicit!r}"
            )
        raise RuntimeError(
            f"Home Assistant has multiple entities with friendly name {explicit!r}: "
            + ", ".join(named_candidates)
        )

    candidates = [
        state.get("entity_id")
        for state in states
        if isinstance(state, dict)
        and isinstance(state.get("entity_id"), str)
        and FOLDER_ENTITY_PATTERN.match(state["entity_id"])
    ]
    if len(candidates) != 1:
        details = ", ".join(candidates) or "none"
        raise RuntimeError(
            "Could not uniquely discover the displayed-folder select; "
            f"candidates: {details}. Use --folder-entity."
        )
    return candidates[0]


def select_folder(home_assistant: str, token: str, entity_id: str, option: str) -> None:
    json_request(
        f"{home_assistant}/api/services/select/select_option",
        "POST",
        token=token,
        body={"entity_id": entity_id, "option": option},
    )


def safe_filename(name: str, fallback: str) -> str:
    cleaned = INVALID_FILENAME_CHARS.sub("_", name).strip().rstrip(".")
    cleaned = re.sub(r"\s+", " ", cleaned)
    return cleaned or fallback


def capture(device: str, output: Path, timeout: float) -> None:
    request(
        f"{device}/screenshot.png",
        accept="application/json",
        timeout=timeout,
        allowed_errors={500, 503},
    )
    deadline = time.monotonic() + timeout
    while True:
        try:
            status, image = request(
                f"{device}/screenshot.png?ts={time.time_ns()}",
                accept="image/png,image/jpeg",
                timeout=timeout,
                allowed_errors={202, 500, 503},
            )
            if status == 200 and image.startswith(b"\x89PNG\r\n\x1a\n"):
                output.write_bytes(image)
                return
        except RuntimeError:
            if time.monotonic() >= deadline:
                raise
        if time.monotonic() >= deadline:
            raise RuntimeError(f"Timed out waiting for screenshot {output.name}")
        time.sleep(0.25)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="HATi device URL")
    parser.add_argument("--home-assistant", required=True, help="Home Assistant URL")
    parser.add_argument(
        "--token", help="Home Assistant token (overrides --secrets)"
    )
    parser.add_argument(
        "--secrets",
        type=Path,
        default=Path(__file__).resolve().parents[2] / "../secrets.yaml",
        help="ESPHome secrets file (default: config/secrets.yaml)",
    )
    parser.add_argument("--device-name", required=True, help="Output directory name")
    parser.add_argument(
        "--folder-entity",
        help="Displayed-folder entity ID or Home Assistant friendly name",
    )
    parser.add_argument(
        "--output-root", type=Path, default=Path("images"), help="Screenshot root (default: images)"
    )
    parser.add_argument(
        "--settle-seconds", type=float, default=DEFAULT_SETTLE_SECONDS,
        help="Delay after selecting each folder (default: 5)",
    )
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    args = parser.parse_args()
    if args.settle_seconds < 0 or args.timeout <= 0:
        parser.error("--settle-seconds must be non-negative and --timeout must be positive")
    args.device = base_url(args.device, "--device")
    args.home_assistant = base_url(args.home_assistant, "--home-assistant")
    args.token = args.token or read_token(args.secrets)
    return args


def main() -> int:
    args = parse_args()
    try:
        folders = fetch_folders(args.device, args.timeout)
        entity_id = discover_folder_entity(args.home_assistant, args.token, args.folder_entity)
        output_dir = args.output_root / safe_filename(args.device_name, "device")
        output_dir.mkdir(parents=True, exist_ok=True)
        print(f"Capturing {len(folders)} folders using {entity_id}")
        for folder in folders:
            filename = safe_filename(folder["name"], f"folder-{folder['id']}") + ".png"
            output = output_dir / filename
            print(f"Selecting {folder['name']} ({folder['id']}) ...", flush=True)
            select_folder(args.home_assistant, args.token, entity_id, folder["name"])
            time.sleep(args.settle_seconds)
            capture(args.device, output, args.timeout)
            print(f"Saved {output}")
    except (OSError, RuntimeError, ValueError, HTTPError, URLError) as exc:
        print(f"Folder screenshot capture failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
