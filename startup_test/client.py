from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys
import time
from pathlib import Path
from typing import Any

from navigation_test.client import (
    ApiError,
    RelayClient,
    build_metrics,
    display_update,
    localization_pose,
    require_safe_transport,
    wait_for_terminal,
)
from navigation_test.metrics import utc_now


DEFAULT_GO2_DEVICE = "ORSUS-GO2-GSM20260003"

def calibrate_or_error(client: RelayClient) -> dict[str, Any]:
    try:
        return client.calibrate_clock()
    except ApiError as exc:
        return {"error": str(exc)}


def write_report(
    output_dir: Path,
    device_id: str,
    command: dict[str, Any],
    observed_at: str,
    client: RelayClient,
    pc_clock: dict[str, Any],
    insecure_http: bool,
) -> tuple[Path, Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%dT%H%M%S", time.localtime())
    stem = f"{stamp}-{device_id}-{str(command.get('command_id', 'unknown'))[:8]}"
    json_path = output_dir / f"{stem}.json"
    csv_path = output_dir / f"{stem}.csv"
    result = command.get("result") if isinstance(command.get("result"), dict) else {}
    localized = localization_pose(result)
    metrics = build_metrics(client, command, observed_at, pc_clock)
    metrics["startup_timing"] = metrics.pop("navigation_timing", None)
    report = {
        "schema_version": 1,
        "demo": "robot_startup_self_check",
        "generated_at": utc_now(),
        "device_id": device_id,
        "passed": (
            command.get("state") == "COMPLETED"
            and result.get("status") == "ready"
            and localized is not None
        ),
        "security": {"insecure_http": insecure_http},
        "command": command,
        "metrics": metrics,
    }
    json_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    rows = list(client.samples)
    device_metrics = result.get("network_metrics")
    if isinstance(device_metrics, dict):
        for sample in device_metrics.get("samples", []):
            rows.append({"source": "orsus_ecs", **sample})
    fieldnames = ["source", "recorded_at", "method", "path", "status_code", "rtt_ms", "success"]
    with csv_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    return json_path, csv_path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Start and self-check an Orsus robot through ECS")
    parser.add_argument(
        "--base-url",
        default=os.environ.get("RELAY_BASE_URL", "http://120.24.74.70"),
    )
    parser.add_argument("--token", default=os.environ.get("RELAY_OPERATOR_TOKEN"))
    parser.add_argument("--allow-insecure-http", action="store_true")
    parser.add_argument("--poll-interval", type=float, default=0.5)
    parser.add_argument("--output-dir", type=Path, default=Path("startup_test/results"))
    subparsers = parser.add_subparsers(dest="action", required=True)

    run = subparsers.add_parser(
        "run",
        help="start robot services, relocalize, and self-check",
    )
    run.add_argument("--device-id", default=os.environ.get("RELAY_DEVICE_ID", DEFAULT_GO2_DEVICE))
    run.add_argument("--request-id")
    run.add_argument("--wait-seconds", type=float, default=600)

    status = subparsers.add_parser("status", help="show one startup command")
    status.add_argument("command_id")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if not args.token or len(args.token) < 32:
        print("error: set RELAY_OPERATOR_TOKEN or pass --token with at least 32 characters", file=sys.stderr)
        return 2
    if args.poll_interval <= 0:
        print("error: --poll-interval must be positive", file=sys.stderr)
        return 2
    if args.action == "run" and args.wait_seconds <= 0:
        print("error: --wait-seconds must be positive", file=sys.stderr)
        return 2

    client = RelayClient(args.base_url, args.token, user_agent="startup-test-client/1.0")
    try:
        insecure = require_safe_transport(args.base_url, args.allow_insecure_http)
        if args.action == "status":
            print(json.dumps(client.command(args.command_id), ensure_ascii=False, indent=2))
            return 0

        pc_clock = calibrate_or_error(client)
        command = client.submit(
            args.device_id,
            "STARTUP",
            {},
            args.request_id,
        )
        print(f"startup command: {command['command_id']}")
        try:
            terminal, observed_at = wait_for_terminal(
                client,
                command["command_id"],
                args.wait_seconds,
                args.poll_interval,
                display_update,
            )
        except KeyboardInterrupt:
            print(
                f"\nlocal wait interrupted; startup continues remotely: {command['command_id']}",
                file=sys.stderr,
            )
            return 130

        json_path, csv_path = write_report(
            args.output_dir,
            args.device_id,
            terminal,
            observed_at,
            client,
            pc_clock,
            insecure,
        )
        result = terminal.get("result")
        if isinstance(result, dict):
            localized = localization_pose(result)
            if localized is not None:
                localization, pose = localized
                theta = float(pose["theta"])
                print(
                    "localized pose: "
                    f"map={localization.get('map', 'unknown')} "
                    f"x={float(pose['x']):.3f} y={float(pose['y']):.3f} "
                    f"theta={theta:.6f} rad ({math.degrees(theta):.2f} deg)"
                )
        if terminal.get("state") == "FAILED":
            print(f"startup failed: {terminal.get('error') or 'unknown error'}", file=sys.stderr)
        print(f"JSON report: {json_path}")
        print(f"CSV samples: {csv_path}")
        return (
            0
            if terminal.get("state") == "COMPLETED"
            and isinstance(result, dict)
            and result.get("status") == "ready"
            and localization_pose(result) is not None
            else 1
        )
    except ApiError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
