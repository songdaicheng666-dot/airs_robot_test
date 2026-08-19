from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path
from typing import Any, Callable

from .metrics import corrected_latency_ms, summarize_rtt, utc_now


TERMINAL_STATES = {"COMPLETED", "FAILED", "CANCELLED"}
DEFAULT_GO2_DEVICE = "ORSUS-GO2-GSM20260003"


class ApiError(RuntimeError):
    def __init__(self, message: str, status_code: int | None = None):
        super().__init__(message)
        self.status_code = status_code


class RelayClient:
    def __init__(
        self,
        base_url: str,
        token: str,
        *,
        timeout: float = 35.0,
        user_agent: str = "navigation-test-client/1.0",
    ):
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.timeout = timeout
        self.user_agent = user_agent
        self.samples: list[dict[str, Any]] = []

    def request_json(
        self,
        method: str,
        path: str,
        payload: dict[str, Any] | None = None,
    ) -> dict[str, Any] | None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8") if payload is not None else None
        request = urllib.request.Request(
            f"{self.base_url}{path}",
            method=method,
            data=body,
            headers={
                "Authorization": f"Bearer {self.token}",
                "Content-Type": "application/json",
                "Accept": "application/json",
                "User-Agent": self.user_agent,
            },
        )
        recorded_at = utc_now()
        started = time.perf_counter()
        success = False
        status_code: int | None = None
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                status_code = response.status
                raw = response.read()
                success = 200 <= response.status < 300
                if response.status == 204 or not raw:
                    return None
                value = json.loads(raw.decode("utf-8"))
                if not isinstance(value, dict):
                    raise ApiError("relay returned a non-object response", response.status)
                return value
        except urllib.error.HTTPError as exc:
            status_code = exc.code
            raw = exc.read().decode("utf-8", errors="replace")
            try:
                detail = json.loads(raw).get("detail", raw)
            except json.JSONDecodeError:
                detail = raw
            raise ApiError(f"HTTP {exc.code}: {detail}", exc.code) from exc
        except urllib.error.URLError as exc:
            raise ApiError(f"request failed: {exc.reason}") from exc
        except json.JSONDecodeError as exc:
            raise ApiError("relay returned invalid JSON", status_code) from exc
        finally:
            self.samples.append(
                {
                    "source": "pc_ecs",
                    "recorded_at": recorded_at,
                    "method": method,
                    "path": path.split("?", 1)[0],
                    "status_code": status_code,
                    "rtt_ms": round((time.perf_counter() - started) * 1000, 3),
                    "success": success,
                }
            )

    def calibrate_clock(self, count: int = 5) -> dict[str, Any]:
        samples: list[dict[str, float]] = []
        for _ in range(count):
            before_ns = time.time_ns()
            response = self.request_json("GET", "/v1/time")
            after_ns = time.time_ns()
            if response is None or not isinstance(response.get("unix_time_ns"), int):
                raise ApiError("relay time endpoint returned invalid data")
            rtt_ms = (after_ns - before_ns) / 1_000_000
            midpoint_ns = (before_ns + after_ns) / 2
            samples.append(
                {
                    "offset_ms": (response["unix_time_ns"] - midpoint_ns) / 1_000_000,
                    "uncertainty_ms": rtt_ms / 2,
                    "rtt_ms": rtt_ms,
                }
            )
        best = min(samples, key=lambda item: item["rtt_ms"])
        return {
            "offset_ms": round(best["offset_ms"], 3),
            "uncertainty_ms": round(best["uncertainty_ms"], 3),
            "sample_count": len(samples),
        }

    def submit(
        self,
        device_id: str,
        command_type: str,
        payload: dict[str, Any],
        request_id: str | None = None,
    ) -> dict[str, Any]:
        value = self.request_json(
            "POST",
            f"/v1/devices/{urllib.parse.quote(device_id, safe='')}/commands",
            {
                "client_request_id": request_id or str(uuid.uuid4()),
                "type": command_type,
                "payload": payload,
            },
        )
        if value is None:
            raise ApiError("relay returned no command")
        return value

    def command(self, command_id: str) -> dict[str, Any]:
        value = self.request_json("GET", f"/v1/commands/{urllib.parse.quote(command_id, safe='')}")
        if value is None:
            raise ApiError("relay returned no command state")
        return value


def wait_for_terminal(
    client: RelayClient,
    command_id: str,
    timeout: float,
    poll_interval: float,
    on_update: Callable[[dict[str, Any]], None] | None = None,
) -> tuple[dict[str, Any], str]:
    deadline = time.monotonic() + timeout
    last_signature: tuple[Any, ...] | None = None
    while True:
        try:
            command = client.command(command_id)
        except ApiError as exc:
            if exc.status_code not in {None, 502, 503, 504}:
                raise
            if time.monotonic() >= deadline:
                raise ApiError(f"command status remained unavailable for {timeout:g} seconds") from exc
            time.sleep(poll_interval)
            continue
        progress = command.get("progress") or {}
        signature = (
            command.get("state"),
            progress.get("phase"),
            progress.get("phase_status"),
            progress.get("mission_id"),
        )
        if signature != last_signature and on_update is not None:
            on_update(command)
            last_signature = signature
        if command.get("state") in TERMINAL_STATES:
            return command, utc_now()
        if time.monotonic() >= deadline:
            raise ApiError(f"command did not finish within {timeout:g} seconds")
        time.sleep(poll_interval)


def display_update(command: dict[str, Any]) -> None:
    progress = command.get("progress") or {}
    text = f"[{command.get('state', 'UNKNOWN')}] {progress.get('phase', 'waiting')}"
    if progress.get("phase_status"):
        text += f": {progress['phase_status']}"
    if progress.get("mission_id"):
        text += f" (mission {progress['mission_id']})"
    print(text, flush=True)


def is_local_http(base_url: str) -> bool:
    parsed = urllib.parse.urlparse(base_url)
    return parsed.scheme == "http" and parsed.hostname in {"127.0.0.1", "localhost", "::1"}


def require_safe_transport(base_url: str, allow_insecure: bool) -> bool:
    parsed = urllib.parse.urlparse(base_url)
    if parsed.scheme == "https" or is_local_http(base_url):
        return False
    if parsed.scheme == "http" and allow_insecure:
        print("WARNING: navigation commands and bearer token are using insecure public HTTP", file=sys.stderr)
        return True
    raise ApiError("public navigation over HTTP requires --allow-insecure-http")


def localization_pose(result: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]] | None:
    localization = result.get("localization")
    pose = localization.get("pose") if isinstance(localization, dict) else None
    if not isinstance(localization, dict) or localization.get("status") != "successful":
        return None
    if not isinstance(pose, dict):
        return None
    for name in ("x", "y", "theta"):
        value = pose.get(name)
        if (
            isinstance(value, bool)
            or not isinstance(value, (int, float))
            or not math.isfinite(value)
        ):
            return None
    return localization, pose


def build_metrics(
    client: RelayClient,
    command: dict[str, Any],
    observed_at: str,
    pc_clock: dict[str, Any],
) -> dict[str, Any]:
    result = command.get("result") if isinstance(command.get("result"), dict) else {}
    communication = (
        result.get("communication_timestamps")
        if isinstance(result.get("communication_timestamps"), dict)
        else {}
    )
    agent_clock = result.get("clock_calibration") if isinstance(result, dict) else {}
    agent_offset = float(agent_clock.get("offset_ms", 0.0)) if isinstance(agent_clock, dict) else 0.0
    pc_offset = float(pc_clock.get("offset_ms", 0.0))
    return {
        "pc_ecs_http": summarize_rtt(client.samples),
        "orsus_ecs_http": result.get("network_metrics") if isinstance(result, dict) else None,
        "clock": {"pc_to_ecs": pc_clock, "orsus_to_ecs": agent_clock},
        "application_latency_ms": {
            "ecs_to_device_command": corrected_latency_ms(
                command.get("created_at"),
                communication.get("device_received_at"),
                end_offset_ms=agent_offset,
            ),
            "device_to_ecs_terminal": corrected_latency_ms(
                communication.get("device_completed_at") or command.get("device_recorded_at"),
                command.get("terminal_at"),
                start_offset_ms=agent_offset,
            ),
            "ecs_to_pc_notification": corrected_latency_ms(
                command.get("terminal_at"),
                observed_at,
                end_offset_ms=pc_offset,
            ),
        },
        "navigation_timing": result.get("timing") if isinstance(result, dict) else None,
    }


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
    report = {
        "schema_version": 1,
        "generated_at": utc_now(),
        "device_id": device_id,
        "passed": command.get("state") == "COMPLETED"
        and isinstance(command.get("result"), dict)
        and command["result"].get("status") == "completed"
        and localization_pose(command["result"]) is not None,
        "security": {"insecure_http": insecure_http},
        "command": command,
        "metrics": build_metrics(client, command, observed_at, pc_clock),
    }
    json_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    rows = list(client.samples)
    result = command.get("result") if isinstance(command.get("result"), dict) else {}
    device_metrics = result.get("network_metrics") if isinstance(result, dict) else None
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
    parser = argparse.ArgumentParser(description="Run Orsus navigation through the ECS relay")
    parser.add_argument(
        "--base-url",
        default=os.environ.get("RELAY_BASE_URL", "http://120.24.74.70"),
    )
    parser.add_argument("--token", default=os.environ.get("RELAY_OPERATOR_TOKEN"))
    parser.add_argument("--allow-insecure-http", action="store_true")
    parser.add_argument("--poll-interval", type=float, default=0.5)
    parser.add_argument("--output-dir", type=Path, default=Path("navigation_test/results"))
    subparsers = parser.add_subparsers(dest="action", required=True)

    run = subparsers.add_parser("run", help="send a single target and wait for arrival")
    run.add_argument("--device-id", default=os.environ.get("RELAY_DEVICE_ID", DEFAULT_GO2_DEVICE))
    run.add_argument("--x", type=float, required=True)
    run.add_argument("--y", type=float, required=True)
    run.add_argument("--theta", type=float, required=True)
    run.add_argument("--request-id")
    run.add_argument("--wait-seconds", type=float, default=3600)

    status = subparsers.add_parser("status", help="show one relay command")
    status.add_argument("command_id")

    cancel = subparsers.add_parser("cancel", help="stop an active navigation command")
    cancel.add_argument("--device-id", default=os.environ.get("RELAY_DEVICE_ID", DEFAULT_GO2_DEVICE))
    cancel.add_argument("navigation_command_id")
    cancel.add_argument("--wait-seconds", type=float, default=30)
    return parser


def _clock_or_error(client: RelayClient) -> dict[str, Any]:
    try:
        return client.calibrate_clock()
    except ApiError as exc:
        return {"error": str(exc)}


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if not args.token or len(args.token) < 32:
        print("error: set RELAY_OPERATOR_TOKEN or pass --token with at least 32 characters", file=sys.stderr)
        return 2
    if args.poll_interval <= 0:
        print("error: --poll-interval must be positive", file=sys.stderr)
        return 2
    if args.action == "run" and not all(math.isfinite(value) for value in (args.x, args.y, args.theta)):
        print("error: --x, --y, and --theta must be finite numbers", file=sys.stderr)
        return 2
    if args.action in {"run", "cancel"} and args.wait_seconds <= 0:
        print("error: --wait-seconds must be positive", file=sys.stderr)
        return 2
    client = RelayClient(args.base_url, args.token)

    try:
        insecure = require_safe_transport(args.base_url, args.allow_insecure_http)
        if args.action == "status":
            print(json.dumps(client.command(args.command_id), ensure_ascii=False, indent=2))
            return 0

        if args.action == "cancel":
            command = client.submit(
                args.device_id,
                "CANCEL_NAVIGATION",
                {"navigation_command_id": args.navigation_command_id},
            )
            terminal, _ = wait_for_terminal(
                client, command["command_id"], args.wait_seconds, args.poll_interval, display_update
            )
            print(json.dumps(terminal, ensure_ascii=False, indent=2))
            return 0 if terminal["state"] == "COMPLETED" else 1

        pc_clock = _clock_or_error(client)
        command = client.submit(
            args.device_id,
            "NAVIGATE",
            {"target": {"x": args.x, "y": args.y, "theta": args.theta}},
            args.request_id,
        )
        print(f"navigation command: {command['command_id']}")
        interrupted = False
        try:
            terminal, observed_at = wait_for_terminal(
                client, command["command_id"], args.wait_seconds, args.poll_interval, display_update
            )
        except KeyboardInterrupt:
            interrupted = True
            print("\ninterrupt received; requesting remote navigation stop", file=sys.stderr)
            cancel = client.submit(
                args.device_id,
                "CANCEL_NAVIGATION",
                {"navigation_command_id": command["command_id"]},
            )
            cancel_terminal, _ = wait_for_terminal(
                client, cancel["command_id"], 30, args.poll_interval, display_update
            )
            if cancel_terminal["state"] != "COMPLETED":
                print("warning: remote stop was not fully confirmed", file=sys.stderr)
            terminal, observed_at = wait_for_terminal(
                client, command["command_id"], 30, args.poll_interval, display_update
            )

        json_path, csv_path = write_report(
            args.output_dir,
            args.device_id,
            terminal,
            observed_at,
            client,
            pc_clock,
            insecure,
        )
        print(f"JSON report: {json_path}")
        print(f"CSV samples: {csv_path}")
        if interrupted:
            return 130
        result = terminal.get("result")
        if terminal.get("state") == "COMPLETED" and isinstance(result, dict) and result.get(
            "status"
        ) == "completed":
            localized = localization_pose(result)
            if localized is None:
                print(
                    "navigation failed at relocalization: "
                    "completed result has no fresh successful localization pose",
                    file=sys.stderr,
                )
                return 1
            localization, pose = localized
            theta = float(pose["theta"])
            print(
                "localized before navigation: "
                f"map={localization.get('map', 'unknown')} "
                f"x={float(pose['x']):.3f} y={float(pose['y']):.3f} "
                f"theta={theta:.6f} rad ({math.degrees(theta):.2f} deg)"
            )
            target = result.get("target") if isinstance(result.get("target"), dict) else {}
            print(
                "navigation completed: "
                f"mission={result.get('mission_id', 'unknown')} "
                f"target=({target.get('x', 'unknown')}, {target.get('y', 'unknown')}, "
                f"{target.get('theta', 'unknown')})"
            )
            return 0
        progress = terminal.get("progress") if isinstance(terminal.get("progress"), dict) else {}
        phase = (
            result.get("failed_phase")
            if isinstance(result, dict) and result.get("failed_phase")
            else progress.get("phase", "unknown")
        )
        detail = terminal.get("error")
        if not detail and isinstance(result, dict):
            detail = result.get("status")
        print(f"navigation failed at {phase}: {detail or 'unknown error'}", file=sys.stderr)
        return 1
    except ApiError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
