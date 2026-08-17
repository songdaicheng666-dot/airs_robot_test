from __future__ import annotations

import json
import os
import signal
import sys
import threading
import time
from datetime import datetime, timezone
from typing import Any

from communication_test.pc.client import ApiError, request_json


def now_text() -> str:
    return datetime.now(tz=timezone.utc).isoformat().replace("+00:00", "Z")


class DeviceEmulator:
    def __init__(self, base_url: str, device_id: str, token: str) -> None:
        self.base_url = base_url.rstrip("/")
        self.device_id = device_id
        self.token = token
        self.stop_event = threading.Event()
        self.sequence = 0
        self.telemetry_lock = threading.Lock()
        self.latest_telemetry: dict[str, Any] = {}

    def telemetry(self) -> dict[str, Any]:
        with self.telemetry_lock:
            self.sequence += 1
            self.latest_telemetry = {
                "recorded_at": now_text(),
                "sequence": self.sequence,
                "psdk_connected": False,
                "flight": {"valid": False},
                "position": {"valid": False},
                "gps": {"valid": False},
                "rtk": {"valid": False},
                "battery": {"valid": False},
                "errors": ["DEVICE_EMULATOR_NO_PSDK"],
            }
            return self.latest_telemetry.copy()

    def post_state(
        self,
        command_id: str,
        state: str,
        result: dict[str, Any] | None = None,
        error: str | None = None,
    ) -> None:
        request_json(
            "POST",
            f"{self.base_url}/v1/devices/{self.device_id}/commands/{command_id}/state",
            self.token,
            {"state": state, "result": result, "error": error},
        )

    def heartbeat_loop(self) -> None:
        while not self.stop_event.is_set():
            try:
                request_json(
                    "POST",
                    f"{self.base_url}/v1/devices/{self.device_id}/telemetry",
                    self.token,
                    self.telemetry(),
                )
            except ApiError as exc:
                print(f"heartbeat failed: {exc}", file=sys.stderr)
            self.stop_event.wait(5)

    def run(self) -> None:
        heartbeat = threading.Thread(target=self.heartbeat_loop, name="emulator-heartbeat", daemon=True)
        heartbeat.start()
        retry_seconds = 1
        while not self.stop_event.is_set():
            try:
                command = request_json(
                    "GET",
                    f"{self.base_url}/v1/devices/{self.device_id}/commands/next?timeout_s=25",
                    self.token,
                    timeout=35,
                )
                retry_seconds = 1
                if command is None:
                    continue
                command_id = command["command_id"]
                self.post_state(command_id, "RECEIVED")
                if command["type"] == "PING":
                    result = {"message": "pong", "echo": command.get("payload", {})}
                elif command["type"] == "STATUS_QUERY":
                    result = {"telemetry": self.telemetry()}
                else:
                    self.post_state(command_id, "FAILED", error="unsupported command type")
                    continue
                self.post_state(command_id, "COMPLETED", result=result)
                print(f"completed {command['type']} {command_id}")
            except ApiError as exc:
                print(f"poll failed: {exc}; retrying in {retry_seconds}s", file=sys.stderr)
                self.stop_event.wait(retry_seconds)
                retry_seconds = min(retry_seconds * 2, 30)


def main() -> int:
    base_url = os.environ.get("M4T_BASE_URL", "http://127.0.0.1:8000")
    device_id = os.environ.get("M4T_DEVICE_ID", "M4T-001")
    token = os.environ.get("M4T_DEVICE_TOKEN", "")
    if len(token) < 32:
        print("error: M4T_DEVICE_TOKEN must contain at least 32 characters", file=sys.stderr)
        return 2
    emulator = DeviceEmulator(base_url, device_id, token)

    def stop(_signal_number: int, _frame: Any) -> None:
        emulator.stop_event.set()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    emulator.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
