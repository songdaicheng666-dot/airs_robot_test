from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys
import time
import uuid
from pathlib import Path
from typing import Any

from navigation_test.client import (
    ApiError,
    RelayClient,
    display_update,
    require_safe_transport,
    wait_for_terminal,
)
from navigation_test.metrics import corrected_latency_ms, summarize_rtt, utc_now


DEFAULT_DEVICE_ID = "M4T-001"
DEFAULT_TARGET_LATITUDE_DEG = 22.604375789
DEFAULT_TARGET_LONGITUDE_DEG = 114.057071644
DEFAULT_TARGET_ALTITUDE_ELLIPSOID_M = 106.0


def write_report(
    output_dir: Path,
    device_id: str,
    action: str,
    command: dict[str, Any],
    observed_at: str,
    client: RelayClient,
    clock: dict[str, Any],
    insecure_http: bool,
) -> tuple[Path, Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%dT%H%M%S", time.localtime())
    stem = f"{stamp}-{device_id}-{action}-{str(command.get('command_id', 'unknown'))[:8]}"
    json_path = output_dir / f"{stem}.json"
    csv_path = output_dir / f"{stem}.csv"
    report = {
        "schema_version": 1,
        "generated_at": utc_now(),
        "device_id": device_id,
        "action": action,
        "passed": command.get("state") == "COMPLETED",
        "security": {"insecure_http": insecure_http},
        "command": command,
        "metrics": {
            "pc_ecs_http": summarize_rtt(client.samples),
            "clock": {"pc_to_ecs": clock},
            "application_latency_ms": {
                "ecs_queue_to_terminal": corrected_latency_ms(
                    command.get("created_at"), command.get("terminal_at")
                ),
                "ecs_to_pc_notification": corrected_latency_ms(
                    command.get("terminal_at"), observed_at,
                    end_offset_ms=float(clock.get("offset_ms", 0.0))
                    if isinstance(clock, dict)
                    else 0.0,
                ),
            },
        },
    }
    json_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    fieldnames = ["source", "recorded_at", "method", "path", "status_code", "rtt_ms", "success"]
    with csv_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(client.samples)
    return json_path, csv_path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Control M4T single-target navigation through the ECS relay")
    parser.add_argument(
        "--base-url",
        default=os.environ.get("RELAY_BASE_URL")
        or os.environ.get("M4T_BASE_URL", "http://120.24.74.70"),
    )
    parser.add_argument(
        "--token",
        default=os.environ.get("RELAY_OPERATOR_TOKEN") or os.environ.get("M4T_OPERATOR_TOKEN"),
    )
    parser.add_argument(
        "--device-id",
        default=os.environ.get("RELAY_DEVICE_ID") or os.environ.get("M4T_DEVICE_ID", DEFAULT_DEVICE_ID),
    )
    parser.add_argument("--allow-insecure-http", action="store_true")
    parser.add_argument("--poll-interval", type=float, default=0.5)
    parser.add_argument("--output-dir", type=Path, default=Path("m4t_navigation_test/results"))
    subparsers = parser.add_subparsers(dest="action", required=True)

    startup = subparsers.add_parser("startup", help="run no-motion checks and create one ready state")
    startup.add_argument("--request-id")
    startup.add_argument("--wait-seconds", type=float, default=60)

    run = subparsers.add_parser("run", help="submit one WGS84 ellipsoid target and wait for arrival")
    run.add_argument("--latitude-deg", type=float, default=DEFAULT_TARGET_LATITUDE_DEG)
    run.add_argument("--longitude-deg", type=float, default=DEFAULT_TARGET_LONGITUDE_DEG)
    run.add_argument(
        "--altitude-ellipsoid-m",
        type=float,
        default=DEFAULT_TARGET_ALTITUDE_ELLIPSOID_M,
    )
    run.add_argument("--request-id")
    run.add_argument("--wait-seconds", type=float, default=3600)

    status = subparsers.add_parser("status", help="show one relay command")
    status.add_argument("command_id")

    cancel = subparsers.add_parser("cancel", help="RTH an active navigation and wait for motors to stop")
    cancel.add_argument("navigation_command_id")
    cancel.add_argument("--request-id")
    cancel.add_argument("--wait-seconds", type=float, default=700)

    return_home = subparsers.add_parser("return-home", help="RTH after arrival and wait for motors to stop")
    return_home.add_argument("--request-id")
    return_home.add_argument("--wait-seconds", type=float, default=700)
    return parser


def _clock_or_error(client: RelayClient) -> dict[str, Any]:
    try:
        return client.calibrate_clock()
    except ApiError as exc:
        return {"error": str(exc)}


def _validate_args(args: argparse.Namespace) -> str | None:
    if not args.token or len(args.token) < 32:
        return "set RELAY_OPERATOR_TOKEN/M4T_OPERATOR_TOKEN or pass --token with at least 32 characters"
    if args.poll_interval <= 0:
        return "--poll-interval must be positive"
    if hasattr(args, "wait_seconds") and args.wait_seconds <= 0:
        return "--wait-seconds must be positive"
    if args.action == "run":
        values = (args.latitude_deg, args.longitude_deg, args.altitude_ellipsoid_m)
        if not all(math.isfinite(value) for value in values):
            return "M4T target coordinates must be finite"
        if not -90 <= args.latitude_deg <= 90:
            return "--latitude-deg must be within [-90, 90]"
        if not -180 <= args.longitude_deg <= 180:
            return "--longitude-deg must be within [-180, 180]"
    return None


def _wait_and_report(
    client: RelayClient,
    args: argparse.Namespace,
    command: dict[str, Any],
    clock: dict[str, Any],
    insecure_http: bool,
) -> dict[str, Any]:
    terminal, observed_at = wait_for_terminal(
        client,
        command["command_id"],
        args.wait_seconds,
        args.poll_interval,
        display_update,
    )
    json_path, csv_path = write_report(
        args.output_dir,
        args.device_id,
        args.action,
        terminal,
        observed_at,
        client,
        clock,
        insecure_http,
    )
    print(json.dumps(terminal, ensure_ascii=False, indent=2))
    print(f"JSON report: {json_path}")
    print(f"CSV samples: {csv_path}")
    return terminal


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    validation_error = _validate_args(args)
    if validation_error:
        print(f"error: {validation_error}", file=sys.stderr)
        return 2
    client = RelayClient(args.base_url, args.token)
    try:
        if args.action == "status":
            print(json.dumps(client.command(args.command_id), ensure_ascii=False, indent=2))
            return 0
        insecure = require_safe_transport(args.base_url, args.allow_insecure_http)
        clock = _clock_or_error(client)
        if args.action == "startup":
            command = client.submit(args.device_id, "STARTUP", {}, args.request_id)
        elif args.action == "run":
            command = client.submit(
                args.device_id,
                "NAVIGATE",
                {
                    "target": {
                        "latitude_deg": args.latitude_deg,
                        "longitude_deg": args.longitude_deg,
                        "altitude_ellipsoid_m": args.altitude_ellipsoid_m,
                    }
                },
                args.request_id,
            )
        elif args.action == "cancel":
            command = client.submit(
                args.device_id,
                "CANCEL_NAVIGATION",
                {"navigation_command_id": args.navigation_command_id},
                args.request_id,
            )
        else:
            command = client.submit(args.device_id, "RETURN_HOME", {}, args.request_id)

        print(f"{args.action} command: {command['command_id']}")
        try:
            terminal = _wait_and_report(client, args, command, clock, insecure)
        except KeyboardInterrupt:
            if args.action != "run":
                raise
            print("\ninterrupt received; requesting M4T cancel/RTH", file=sys.stderr)
            cancel = client.submit(
                args.device_id,
                "CANCEL_NAVIGATION",
                {"navigation_command_id": command["command_id"]},
                str(uuid.uuid4()),
            )
            cancel_args = argparse.Namespace(**vars(args))
            cancel_args.action = "cancel"
            cancel_args.wait_seconds = 700
            _wait_and_report(client, cancel_args, cancel, clock, insecure)
            return 130
        return 0 if terminal.get("state") == "COMPLETED" else 1
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130
    except ApiError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
