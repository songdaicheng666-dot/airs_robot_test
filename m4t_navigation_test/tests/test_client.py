from __future__ import annotations

import json
from pathlib import Path

from m4t_navigation_test import client


TOKEN = "operator-token-000000000000000000000000"


def test_parser_builds_geodetic_target(monkeypatch, tmp_path: Path) -> None:
    submitted: list[tuple[str, str, dict, str | None]] = []

    class FakeClient:
        def __init__(self, base_url: str, token: str) -> None:
            self.samples = []

        def calibrate_clock(self):
            return {"offset_ms": 0.0}

        def submit(self, device_id: str, command_type: str, payload: dict, request_id=None):
            submitted.append((device_id, command_type, payload, request_id))
            return {"command_id": "command-1"}

    monkeypatch.setattr(client, "RelayClient", FakeClient)
    monkeypatch.setattr(
        client,
        "wait_for_terminal",
        lambda *_args, **_kwargs: (
            {
                "command_id": "command-1",
                "state": "COMPLETED",
                "created_at": "2026-01-01T00:00:00Z",
                "terminal_at": "2026-01-01T00:00:01Z",
                "result": {"status": "arrived"},
            },
            "2026-01-01T00:00:01Z",
        ),
    )
    result = client.main(
        [
            "--base-url",
            "https://ecs.test",
            "--token",
            TOKEN,
            "--output-dir",
            str(tmp_path),
            "run",
            "--latitude-deg",
            "22.5",
            "--longitude-deg",
            "113.9",
            "--altitude-ellipsoid-m",
            "52",
        ]
    )
    assert result == 0
    assert submitted == [
        (
            "M4T-001",
            "NAVIGATE",
            {
                "target": {
                    "latitude_deg": 22.5,
                    "longitude_deg": 113.9,
                    "altitude_ellipsoid_m": 52.0,
                }
            },
            None,
        )
    ]
    report = json.loads(next(tmp_path.glob("*.json")).read_text(encoding="utf-8"))
    assert report["passed"] is True
    assert report["action"] == "run"


def test_rejects_invalid_latitude(capsys) -> None:
    result = client.main(
        [
            "--token",
            TOKEN,
            "run",
            "--latitude-deg",
            "91",
            "--longitude-deg",
            "113.9",
            "--altitude-ellipsoid-m",
            "52",
        ]
    )
    assert result == 2
    assert "latitude" in capsys.readouterr().err
