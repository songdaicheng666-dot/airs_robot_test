#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import re
import tempfile
from pathlib import Path


TOKEN_PATTERN = re.compile(r"^[A-Za-z0-9._~-]{32,192}$")


def read_env(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ValueError(f"{path}:{line_number}: expected NAME=VALUE")
        key, value = line.split("=", 1)
        values[key] = value
    return values


def validate_token(name: str, token: str) -> str:
    if not TOKEN_PATTERN.fullmatch(token):
        raise ValueError(f"{name} must contain 32-192 safe token characters")
    return token


def write_private_json(path: Path, value: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(value, output, indent=2, sort_keys=True)
            output.write("\n")
        os.replace(temporary_name, path)
    except BaseException:
        try:
            os.close(descriptor)
        except OSError:
            pass
        try:
            os.unlink(temporary_name)
        except OSError:
            pass
        raise


def write_private_text(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            output.write(value)
        os.replace(temporary_name, path)
    except BaseException:
        try:
            os.close(descriptor)
        except OSError:
            pass
        try:
            os.unlink(temporary_name)
        except OSError:
            pass
        raise


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Build a private M4T and Orsus device registry")
    parser.add_argument("--env-file", type=Path, required=True, help="existing m4t-relay.env")
    parser.add_argument("--orsus-token-file", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--orsus-env-output", type=Path)
    parser.add_argument("--orsus-device-id", default="ORSUS-GO2-GSM20260003")
    parser.add_argument("--orsus-sn", default="GSM20260003")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    environment = read_env(args.env_file)
    m4t_token = validate_token("M4T_DEVICE_TOKEN", environment.get("M4T_DEVICE_TOKEN", ""))
    orsus_token = validate_token(
        "Orsus device token",
        args.orsus_token_file.read_text(encoding="utf-8").strip(),
    )
    if m4t_token == orsus_token:
        raise ValueError("M4T and Orsus device tokens must be different")
    registry = {
        "M4T-001": {"kind": "m4t", "token": m4t_token},
        args.orsus_device_id: {
            "kind": "orsus",
            "token": orsus_token,
            "expected_sn": args.orsus_sn,
        },
    }
    write_private_json(args.output, registry)
    if args.orsus_env_output:
        write_private_text(
            args.orsus_env_output,
            "\n".join(
                [
                    "RELAY_BASE_URL=http://120.24.74.70",
                    f"RELAY_DEVICE_ID={args.orsus_device_id}",
                    f"RELAY_DEVICE_TOKEN={orsus_token}",
                    f"ORSUS_EXPECTED_SN={args.orsus_sn}",
                    "ORSUS_BASE_URL=http://127.0.0.1:8898",
                    "ORSUS_NETWORK_INTERFACE=eth3",
                    "RELAY_HEARTBEAT_SECONDS=5",
                    "RELAY_POLL_SECONDS=25",
                    "RELAY_CONNECT_TIMEOUT_SECONDS=3",
                    "RELAY_READ_TIMEOUT_SECONDS=8",
                    "LOG_LEVEL=INFO",
                    "",
                ]
            ),
        )
    print(f"wrote private registry for {len(registry)} devices to {args.output}")
    if args.orsus_env_output:
        print(f"wrote private Orsus environment to {args.orsus_env_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
