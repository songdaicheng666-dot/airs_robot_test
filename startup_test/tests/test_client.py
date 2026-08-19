from __future__ import annotations

import json
from datetime import datetime, timezone
from typing import Any
from unittest.mock import patch

import pytest

from startup_test.client import main


TOKEN = "operator-token-000000000000000000000000"


class FakeHttpResponse:
    def __init__(self, payload: dict[str, Any], status: int = 200):
        self.payload = payload
        self.status = status

    def read(self) -> bytes:
        return json.dumps(self.payload).encode("utf-8")

    def __enter__(self) -> "FakeHttpResponse":
        return self

    def __exit__(self, *_args: Any) -> None:
        return None


def test_startup_command_writes_ready_report_without_target(
    tmp_path: Any,
    capsys: pytest.CaptureFixture[str],
) -> None:
    now = datetime.now(tz=timezone.utc).isoformat().replace("+00:00", "Z")
    command_id = "12345678-1234-1234-1234-123456789abc"
    status_calls = 0

    def urlopen(request: Any, timeout: float) -> FakeHttpResponse:
        nonlocal status_calls
        assert timeout > 0
        assert request.headers["User-agent"] == "startup-test-client/1.0"
        if request.full_url.endswith("/v1/time"):
            return FakeHttpResponse({"server_time": now, "unix_time_ns": __import__("time").time_ns()})
        if request.method == "POST" and request.full_url.endswith("/commands"):
            body = json.loads(request.data.decode("utf-8"))
            assert body["type"] == "STARTUP"
            assert body["payload"] == {}
            return FakeHttpResponse(
                {"command_id": command_id, "state": "QUEUED", "created_at": now},
                201,
            )
        if request.method == "GET" and request.full_url.endswith(f"/v1/commands/{command_id}"):
            status_calls += 1
            if status_calls == 1:
                return FakeHttpResponse(
                    {
                        "command_id": command_id,
                        "state": "RUNNING",
                        "created_at": now,
                        "progress": {"operation": "startup", "phase": "motion", "phase_status": "started"},
                    }
                )
            return FakeHttpResponse(
                {
                    "command_id": command_id,
                    "state": "COMPLETED",
                    "created_at": now,
                    "terminal_at": now,
                    "result": {
                        "operation": "startup",
                        "status": "ready",
                        "mission_id": None,
                        "target": None,
                        "localization": {
                            "status": "successful",
                            "map": "airs1f_3",
                            "mode": "sequential",
                            "pose": {
                                "frame_id": "GSM20260003/map",
                                "child_frame_id": "GSM20260003/base_footprint",
                                "x": 0.994,
                                "y": -0.344,
                                "theta": -0.072,
                                "source_recorded_at": now,
                                "pose_received_at": now,
                            },
                        },
                        "communication_timestamps": {
                            "device_received_at": now,
                            "device_completed_at": now,
                        },
                        "clock_calibration": {"offset_ms": 0, "uncertainty_ms": 1},
                        "network_metrics": {"samples": [], "rtt_ms": {}},
                        "timing": {
                            "total_seconds": 1,
                            "phase_seconds": {"relocalization": 1},
                        },
                        "startup_report": {"ok": True},
                    },
                }
            )
        raise AssertionError(f"unexpected request: {request.method} {request.full_url}")

    with patch("navigation_test.client.urllib.request.urlopen", side_effect=urlopen):
        arguments = [
            "--base-url",
            "https://relay.example",
            "--token",
            TOKEN,
            "--poll-interval",
            "0.001",
            "--output-dir",
            str(tmp_path),
            "run",
            "--device-id",
            "ORSUS-GO2-GSM20260003",
        ]
        exit_code = main(arguments)

    assert exit_code == 0
    assert "theta=-0.072000 rad (-4.13 deg)" in capsys.readouterr().out
    report_path = next(tmp_path.glob("*.json"))
    report = json.loads(report_path.read_text(encoding="utf-8"))
    assert report["demo"] == "robot_startup_self_check"
    assert report["passed"] is True
    assert report["command"]["result"]["target"] is None
    assert report["command"]["result"]["mission_id"] is None
    assert report["command"]["result"]["localization"]["pose"]["x"] == 0.994
    assert report["metrics"]["startup_timing"]["phase_seconds"]["relocalization"] == 1
    assert len(list(tmp_path.glob("*.csv"))) == 1


def test_startup_failure_prints_relay_error(
    tmp_path: Any,
    capsys: pytest.CaptureFixture[str],
) -> None:
    now = datetime.now(tz=timezone.utc).isoformat().replace("+00:00", "Z")
    command_id = "12345678-1234-1234-1234-123456789abc"

    def urlopen(request: Any, timeout: float) -> FakeHttpResponse:
        del timeout
        if request.full_url.endswith("/v1/time"):
            return FakeHttpResponse({"server_time": now, "unix_time_ns": __import__("time").time_ns()})
        if request.method == "POST" and request.full_url.endswith("/commands"):
            return FakeHttpResponse(
                {"command_id": command_id, "state": "QUEUED", "created_at": now},
                201,
            )
        if request.method == "GET" and request.full_url.endswith(f"/v1/commands/{command_id}"):
            return FakeHttpResponse(
                {
                    "command_id": command_id,
                    "state": "FAILED",
                    "created_at": now,
                    "terminal_at": now,
                    "error": "go2: global relocalization failed",
                    "result": {"operation": "startup", "status": "failed"},
                }
            )
        raise AssertionError(f"unexpected request: {request.method} {request.full_url}")

    with patch("navigation_test.client.urllib.request.urlopen", side_effect=urlopen):
        exit_code = main(
            [
                "--base-url",
                "https://relay.example",
                "--token",
                TOKEN,
                "--output-dir",
                str(tmp_path),
                "run",
            ]
        )

    assert exit_code == 1
    assert "startup failed: go2: global relocalization failed" in capsys.readouterr().err


def test_relocalize_option_was_removed() -> None:
    with pytest.raises(SystemExit):
        main(
            [
                "--base-url",
                "https://relay.example",
                "--token",
                TOKEN,
                "run",
                "--relocalize",
            ]
        )
