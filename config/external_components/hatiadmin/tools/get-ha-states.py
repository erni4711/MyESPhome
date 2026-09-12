#!/usr/bin/env python3
"""Fetch Home Assistant entity states through its WebSocket API.

The ``websocket-client`` package is required:

    python -m pip install websocket-client

Example:

    python get-ha-states.py \
        --url http://homeassistant.local:8123 \
        --output states.json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit, urlunsplit

try:
    import websocket
except ImportError as exc:  # pragma: no cover - exercised by the CLI
    raise SystemExit(
        "Missing dependency: install it with "
        "'python -m pip install websocket-client'"
    ) from exc


GET_STATES_ID = 1
SECRET_KEYS = (
    "ha_long_lived_access_token",
    "home_assistant_long_lived_token",
    "ha_long_lived_token",
    "HA_TOKEN",
)


def websocket_url(home_assistant_url: str) -> str:
    """Convert an HTTP(S) Home Assistant URL to its WebSocket endpoint."""
    parsed = urlsplit(home_assistant_url)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise ValueError("URL must include an http:// or https:// scheme and host")

    scheme = "wss" if parsed.scheme == "https" else "ws"
    return urlunsplit((scheme, parsed.netloc, "/api/websocket", "", ""))


def receive_json(connection: Any) -> dict[str, Any]:
    """Receive one text frame and decode it as a JSON object."""
    message = connection.recv()
    if message is None:
        raise RuntimeError("Home Assistant closed the WebSocket connection")
    if isinstance(message, bytes):
        message = message.decode("utf-8")
    decoded = json.loads(message)
    if not isinstance(decoded, dict):
        raise RuntimeError("Home Assistant returned a non-object WebSocket message")
    return decoded


def read_token(secrets_path: Path) -> str:
    """Read the Home Assistant token from an ESPHome secrets file."""
    try:
        lines = secrets_path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise RuntimeError(f"Could not read secrets file {secrets_path}: {exc}") from exc

    values: dict[str, str] = {}
    for line in lines:
        match = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_-]*)\s*:\s*(.*?)\s*(?:#.*)?$", line)
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


def fetch_states(home_assistant_url: str, token: str, timeout: float) -> list[Any]:
    """Authenticate and return the result of a Home Assistant get_states call."""
    connection = websocket.create_connection(
        websocket_url(home_assistant_url),
        timeout=timeout,
    )
    try:
        greeting = receive_json(connection)
        if greeting.get("type") != "auth_required":
            raise RuntimeError(
                "Expected Home Assistant auth_required message, "
                f"got {greeting.get('type', '<missing>')!r}"
            )

        connection.send(json.dumps({"type": "auth", "access_token": token}))
        auth_result = receive_json(connection)
        if auth_result.get("type") == "auth_invalid":
            raise RuntimeError("Home Assistant rejected the access token")
        if auth_result.get("type") != "auth_ok":
            raise RuntimeError(
                "Expected Home Assistant auth_ok message, "
                f"got {auth_result.get('type', '<missing>')!r}"
            )

        connection.send(json.dumps({"id": GET_STATES_ID, "type": "get_states"}))
        result = receive_json(connection)
        if result.get("type") != "result" or result.get("id") != GET_STATES_ID:
            raise RuntimeError("Home Assistant returned an unexpected get_states response")
        if not result.get("success"):
            error = result.get("error")
            raise RuntimeError(f"Home Assistant get_states failed: {error!r}")

        states = result.get("result")
        if not isinstance(states, list):
            raise RuntimeError("Home Assistant get_states response did not contain a list")
        return states
    finally:
        connection.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--url",
        required=True,
        help="Home Assistant base URL, for example http://homeassistant.local:8123",
    )
    parser.add_argument(
        "--token",
        help="Home Assistant long-lived access token (overrides --secrets)",
    )
    parser.add_argument(
        "--secrets",
        type=Path,
        default=Path(__file__).resolve().parents[3] / "secrets.yaml",
        help="ESPHome secrets file (default: config/secrets.yaml)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="Path to the JSON file to write",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=30.0,
        help="Connection/read timeout in seconds (default: 30)",
    )
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    return args


def main() -> int:
    args = parse_args()
    try:
        token = args.token or read_token(args.secrets)
        states = fetch_states(args.url, token, args.timeout)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(states, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError, websocket.WebSocketException) as exc:
        print(f"Failed to fetch Home Assistant states: {exc}", file=sys.stderr)
        return 1

    print(f"Stored {len(states)} Home Assistant states in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
