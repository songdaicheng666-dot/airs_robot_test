from __future__ import annotations

import json
from datetime import datetime, timezone
from typing import Any
from unittest.mock import patch

import pytest

from navigation_test.client import ApiError, main, require_safe_transport
from navigation_test.metrics import corrected_latency_ms, summarize_rtt


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


def test_metrics_summary_and_clock_correction() -> None:
    summary = summarize_rtt(
        [
            {"rtt_ms": 10, "success": True},
            {"rtt_ms": 20, "success": True},
            {"rtt_ms": 999, "success": False},
        ]
    )
    assert summary["sample_count"] == 3
    assert summary["failure_count"] == 1
    assert summary["p50"] == 15
    assert corrected_latency_ms(
        "2026-01-01T00:00:00Z",
        "2026-01-01T00:00:00.100Z",
        end_offset_ms=-10,
    ) == 90


def test_public_http_requires_explicit_flag() -> None:
    with pytest.raises(ApiError, match="requires --allow-insecure-http"):
        require_safe_transport("http://example.test", False)
    assert require_safe_transport("http://example.test", True) is True
    assert require_safe_transport("http://127.0.0.1:8000", False) is False
    assert require_safe_transport("https://example.test", False) is False


@pytest.mark.parametrize(
    ("argument", "value"),
    [("--x", "nan"), ("--y", "inf"), ("--theta", "nan")],
)
def test_run_rejects_non_finite_coordinates(argument: str, value: str) -> None:
    coordinates = {"--x": "1", "--y": "2", "--theta": "0.5"}
    coordinates[argument] = value

    exit_code = main(
        [
            "--base-url",
            "https://relay.example",
            "--token",
            TOKEN,
            "run",
            "--x",
            coordinates["--x"],
            "--y",
            coordinates["--y"],
            "--theta",
            coordinates["--theta"],
        ]
    )

    assert exit_code == 2


def test_status_requires_insecure_http_opt_in() -> None:
    exit_code = main(
        [
            "--base-url",
            "http://relay.example",
            "--token",
            TOKEN,
            "status",
            "command-id",
        ]
    )

    assert exit_code == 1


def test_run_command_writes_json_and_csv_report(tmp_path: Any) -> None:
    now = datetime.now(tz=timezone.utc).isoformat().replace("+00:00", "Z")
    command_id = "12345678-1234-1234-1234-123456789abc"
    status_calls = 0

    def urlopen(request: Any, timeout: float) -> FakeHttpResponse:
        nonlocal status_calls
        assert timeout > 0
        if request.full_url.endswith("/v1/time"):
            return FakeHttpResponse(
                {
                    "server_time": now,
                    "unix_time_ns": __import__("time").time_ns(),
                }
            )
        if request.method == "POST" and request.full_url.endswith("/commands"):
            body = json.loads(request.data.decode("utf-8"))
            assert body["type"] == "NAVIGATE"
            return FakeHttpResponse(
                {
                    "command_id": command_id,
                    "state": "QUEUED",
                    "created_at": now,
                },
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
                        "progress": {"phase": "mission", "phase_status": "running"},
                    }
                )
            return FakeHttpResponse(
                {
                    "command_id": command_id,
                    "state": "COMPLETED",
                    "created_at": now,
                    "terminal_at": now,
                    "result": {
                        "status": "completed",
                        "mission_id": "mission-1",
                        "communication_timestamps": {
                            "device_received_at": now,
                            "device_completed_at": now,
                        },
                        "clock_calibration": {"offset_ms": 0, "uncertainty_ms": 1},
                        "network_metrics": {"samples": [], "rtt_ms": {}},
                        "timing": {"total_seconds": 1, "phase_seconds": {"mission": 1}},
                    },
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
                "--poll-interval",
                "0.001",
                "--output-dir",
                str(tmp_path),
                "run",
                "--device-id",
                "ORSUS-GO2-GSM20260003",
                "--x",
                "1",
                "--y",
                "2",
                "--theta",
                "0.5",
            ]
        )

    assert exit_code == 0
    reports = list(tmp_path.glob("*.json"))
    samples = list(tmp_path.glob("*.csv"))
    assert len(reports) == 1
    assert len(samples) == 1
    report = json.loads(reports[0].read_text(encoding="utf-8"))
    assert report["passed"] is True
    assert report["command"]["result"]["mission_id"] == "mission-1"
    assert report["metrics"]["pc_ecs_http"]["sample_count"] >= 8
