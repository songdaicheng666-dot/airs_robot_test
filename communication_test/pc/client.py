from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path
from typing import Any


class ApiError(RuntimeError):
    pass


def request_json(
    method: str,
    url: str,
    token: str,
    payload: dict[str, Any] | None = None,
    timeout: float = 35.0,
) -> dict[str, Any] | None:
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8") if payload is not None else None
    request = urllib.request.Request(
        url,
        method=method,
        data=body,
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "Accept": "application/json",
            "User-Agent": "m4t-operator-client/1.0",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read()
            if response.status == 204 or not raw:
                return None
            return json.loads(raw.decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        try:
            detail = json.loads(raw).get("detail", raw)
        except json.JSONDecodeError:
            detail = raw
        raise ApiError(f"HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise ApiError(f"request failed: {exc.reason}") from exc
    except json.JSONDecodeError as exc:
        raise ApiError("server returned invalid JSON") from exc


def print_json(value: dict[str, Any] | None) -> None:
    print(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True))


def load_command(args: argparse.Namespace) -> dict[str, Any]:
    if args.json_file:
        try:
            value = json.loads(Path(args.json_file).read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise ApiError(f"could not read command JSON: {exc}") from exc
        if not isinstance(value, dict):
            raise ApiError("command JSON must contain an object")
    else:
        try:
            payload = json.loads(args.payload)
        except json.JSONDecodeError as exc:
            raise ApiError(f"--payload is not valid JSON: {exc}") from exc
        if not isinstance(payload, dict):
            raise ApiError("--payload must contain a JSON object")
        value = {"type": args.type, "payload": payload}

    if value.get("type") not in {"PING", "STATUS_QUERY"}:
        raise ApiError("command type must be PING or STATUS_QUERY")
    if not isinstance(value.get("payload", {}), dict):
        raise ApiError("command payload must be an object")
    value.setdefault("payload", {})
    value.setdefault("client_request_id", str(uuid.uuid4()))
    return value


def wait_for_terminal(base_url: str, token: str, command_id: str, timeout: float) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while True:
        command = request_json("GET", f"{base_url}/v1/commands/{command_id}", token)
        assert command is not None
        if command.get("state") in {"COMPLETED", "FAILED"}:
            return command
        if time.monotonic() >= deadline:
            raise ApiError(f"command did not finish within {timeout:g} seconds")
        time.sleep(1)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Operate devices through the shared cloud relay")
    parser.add_argument(
        "--base-url",
        default=os.environ.get("RELAY_BASE_URL")
        or os.environ.get("M4T_BASE_URL", "http://120.24.74.70"),
        help="relay base URL (default: RELAY_BASE_URL, M4T_BASE_URL, or the test ECS address)",
    )
    parser.add_argument(
        "--token",
        default=os.environ.get("RELAY_OPERATOR_TOKEN") or os.environ.get("M4T_OPERATOR_TOKEN"),
        help="operator bearer token",
    )
    parser.add_argument(
        "--device-id",
        default=os.environ.get("RELAY_DEVICE_ID") or os.environ.get("M4T_DEVICE_ID", "M4T-001"),
    )
    subparsers = parser.add_subparsers(dest="action", required=True)

    send = subparsers.add_parser("send", help="submit a PING or STATUS_QUERY command")
    source = send.add_mutually_exclusive_group(required=True)
    source.add_argument("--json-file", help="path to a structured command JSON file")
    source.add_argument("--type", choices=("PING", "STATUS_QUERY"))
    send.add_argument("--payload", default="{}", help="JSON object used with --type")
    send.add_argument("--wait-seconds", type=float, default=60.0)
    send.add_argument("--no-wait", action="store_true")

    subparsers.add_parser("devices", help="list all configured devices")
    subparsers.add_parser("status", help="read latest device status")
    command = subparsers.add_parser("command", help="read one command by ID")
    command.add_argument("command_id")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if not args.token or len(args.token) < 32:
        print(
            "error: set RELAY_OPERATOR_TOKEN/M4T_OPERATOR_TOKEN or pass a token containing at least 32 characters",
            file=sys.stderr,
        )
        return 2
    base_url = args.base_url.rstrip("/")
    try:
        if args.action == "devices":
            print_json(request_json("GET", f"{base_url}/v1/devices", args.token))
            return 0
        if args.action == "status":
            print_json(request_json("GET", f"{base_url}/v1/devices/{args.device_id}/status", args.token))
            return 0
        if args.action == "command":
            print_json(request_json("GET", f"{base_url}/v1/commands/{args.command_id}", args.token))
            return 0

        command_request = load_command(args)
        command = request_json(
            "POST",
            f"{base_url}/v1/devices/{args.device_id}/commands",
            args.token,
            command_request,
        )
        assert command is not None
        if args.no_wait:
            print_json(command)
            return 0
        completed = wait_for_terminal(base_url, args.token, command["command_id"], args.wait_seconds)
        print_json(completed)
        return 0 if completed["state"] == "COMPLETED" else 1
    except ApiError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
