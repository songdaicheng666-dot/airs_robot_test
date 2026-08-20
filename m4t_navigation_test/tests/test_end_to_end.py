from __future__ import annotations

import json
import threading
import time
import urllib.parse
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any
from unittest.mock import patch

from communication_test.cloud.app import create_app
from communication_test.cloud.config import DeviceSettings, Settings
from m4t_navigation_test.client import main
from navigation_test.tests.test_end_to_end import UrlopenResponse, call_asgi


OPERATOR_TOKEN = "operator-token-000000000000000000000000"
DEVICE_TOKEN = "m4t-token-0000000000000000000000000000"
DEVICE_ID = "M4T-001"
AIRCRAFT_SN = "1581F6M4TTEST0001"
SESSION_ID = "m4t-e2e-flight-session"


def auth(token: str) -> dict[str, str]:
    return {"Authorization": f"Bearer {token}", "Content-Type": "application/json"}


def request(app: Any, method: str, path: str, token: str, body: dict | None = None):
    content = json.dumps(body).encode("utf-8") if body is not None else None
    return call_asgi(app, method, path, auth(token), content)


def telemetry(sequence: int = 1, mission_active: bool = False) -> dict[str, Any]:
    return {
        "recorded_at": datetime.now(tz=timezone.utc).isoformat(),
        "sequence": sequence,
        "psdk_connected": True,
        "flight": {"valid": True, "status_code": 1, "status": "ON_GROUND", "display_mode_code": 6},
        "position": {
            "valid": True,
            "latitude_deg": 22.5,
            "longitude_deg": 113.9,
            "altitude_ellipsoid_m": 42.0,
            "visible_satellites": 18,
        },
        "gps": {
            "valid": True,
            "fix_state": 3,
            "horizontal_accuracy_m": 0.5,
            "vertical_accuracy_m": 0.8,
            "satellites_used": 15,
        },
        "rtk": {"valid": False},
        "battery": {"valid": True, "percentage": 87, "voltage_v": 51.2, "current_a": -1.4},
        "aircraft": {"model": "M4T", "sn": AIRCRAFT_SN},
        "velocity": {"valid": True, "x_mps": 0, "y_mps": 0, "z_mps": 0, "horizontal_speed_mps": 0},
        "home": {
            "is_set": True,
            "latitude_deg": 22.5,
            "longitude_deg": 113.9,
            "altitude_ellipsoid_m": 42.0,
        },
        "rth": {"altitude_m": 50, "active": False},
        "obstacle_avoidance": {"enabled": True},
        "session_id": SESSION_ID,
        "safety": {"navigation_enabled": True, "coordinate_units_verified": True},
        "mission": {"active": mission_active, "phase": "enroute" if mission_active else "idle"},
        "errors": [],
    }


def make_app(tmp_path: Path):
    return create_app(
        Settings(
            database_path=tmp_path / "relay.db",
            operator_token=OPERATOR_TOKEN,
            command_lease_seconds=3,
            command_ttl_seconds=60,
            max_poll_seconds=2,
            allow_insecure_navigation=True,
            devices={
                DEVICE_ID: DeviceSettings(
                    DEVICE_ID, "m4t", DEVICE_TOKEN, expected_sn=AIRCRAFT_SN
                )
            },
        )
    )


def state(app: Any, command_id: str, value: str, **fields: Any) -> None:
    response = request(
        app,
        "POST",
        f"/v1/devices/{DEVICE_ID}/commands/{command_id}/state",
        DEVICE_TOKEN,
        {"state": value, **fields},
    )
    assert response.status_code == 200


def urlopen_for(app: Any):
    def urlopen(request_value: Any, timeout: float) -> UrlopenResponse:
        del timeout
        parsed = urllib.parse.urlsplit(request_value.full_url)
        path = urllib.parse.urlunsplit(("", "", parsed.path, parsed.query, ""))
        headers = {key: value for key, value in request_value.header_items()}
        return UrlopenResponse(
            call_asgi(app, request_value.method, path, headers, request_value.data)
        )

    return urlopen


def test_pc_ecs_fake_m4t_startup_navigation_and_reports(tmp_path: Path) -> None:
    app = make_app(tmp_path)
    assert request(app, "POST", f"/v1/devices/{DEVICE_ID}/telemetry", DEVICE_TOKEN, telemetry()).status_code == 202

    def device_loop() -> None:
        completed = 0
        deadline = time.monotonic() + 8
        while completed < 2 and time.monotonic() < deadline:
            response = request(
                app,
                "GET",
                f"/v1/devices/{DEVICE_ID}/commands/next?timeout_s=0",
                DEVICE_TOKEN,
            )
            if response.status_code == 204:
                time.sleep(0.01)
                continue
            command = response.json()
            command_id = command["command_id"]
            state(app, command_id, "RECEIVED")
            if command["type"] == "STARTUP":
                state(
                    app,
                    command_id,
                    "COMPLETED",
                    result={"status": "ready", "session_id": SESSION_ID, "aircraft_sn": AIRCRAFT_SN},
                )
            else:
                state(app, command_id, "RUNNING", progress={"phase": "enroute", "remaining_distance_m": 5})
                state(app, command_id, "COMPLETED", result={"status": "arrived", "code_name": 7})
            completed += 1
        assert completed == 2

    worker = threading.Thread(target=device_loop, daemon=True)
    worker.start()
    reports = tmp_path / "reports"
    common = [
        "--base-url",
        "http://relay.test",
        "--token",
        OPERATOR_TOKEN,
        "--allow-insecure-http",
        "--poll-interval",
        "0.01",
        "--output-dir",
        str(reports),
    ]
    with patch("navigation_test.client.urllib.request.urlopen", side_effect=urlopen_for(app)):
        assert main([*common, "startup"]) == 0
        assert main(
            [
                *common,
                "run",
                "--latitude-deg",
                "22.5001",
                "--longitude-deg",
                "113.9001",
                "--altitude-ellipsoid-m",
                "52",
            ]
        ) == 0
    worker.join(timeout=3)
    json_reports = sorted(reports.glob("*.json"))
    csv_reports = sorted(reports.glob("*.csv"))
    assert len(json_reports) == 2
    assert len(csv_reports) == 2
    assert {json.loads(path.read_text(encoding="utf-8"))["action"] for path in json_reports} == {
        "startup",
        "run",
    }


def test_cancel_is_delivered_while_navigation_is_running(tmp_path: Path) -> None:
    app = make_app(tmp_path)
    assert request(app, "POST", f"/v1/devices/{DEVICE_ID}/telemetry", DEVICE_TOKEN, telemetry()).status_code == 202

    startup = request(
        app,
        "POST",
        f"/v1/devices/{DEVICE_ID}/commands",
        OPERATOR_TOKEN,
        {"client_request_id": str(uuid.uuid4()), "type": "STARTUP", "payload": {}},
    ).json()
    request(app, "GET", f"/v1/devices/{DEVICE_ID}/commands/next?timeout_s=0", DEVICE_TOKEN)
    state(
        app,
        startup["command_id"],
        "COMPLETED",
        result={"status": "ready", "session_id": SESSION_ID, "aircraft_sn": AIRCRAFT_SN},
    )
    navigation = request(
        app,
        "POST",
        f"/v1/devices/{DEVICE_ID}/commands",
        OPERATOR_TOKEN,
        {
            "client_request_id": str(uuid.uuid4()),
            "type": "NAVIGATE",
            "payload": {
                "target": {
                    "latitude_deg": 22.5001,
                    "longitude_deg": 113.9001,
                    "altitude_ellipsoid_m": 52,
                }
            },
        },
    ).json()
    request(app, "GET", f"/v1/devices/{DEVICE_ID}/commands/next?timeout_s=0", DEVICE_TOKEN)
    state(app, navigation["command_id"], "RUNNING", progress={"phase": "enroute"})

    def cancel_worker() -> None:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            response = request(
                app,
                "GET",
                f"/v1/devices/{DEVICE_ID}/commands/next?timeout_s=0",
                DEVICE_TOKEN,
            )
            if response.status_code == 204:
                time.sleep(0.01)
                continue
            command = response.json()
            assert command["type"] == "CANCEL_NAVIGATION"
            assert command["payload"]["navigation_command_id"] == navigation["command_id"]
            state(app, navigation["command_id"], "CANCELLED", result={"status": "landed"})
            state(app, command["command_id"], "COMPLETED", result={"status": "landed"})
            return
        raise AssertionError("cancel command was not delivered")

    worker = threading.Thread(target=cancel_worker, daemon=True)
    worker.start()
    with patch("navigation_test.client.urllib.request.urlopen", side_effect=urlopen_for(app)):
        exit_code = main(
            [
                "--base-url",
                "http://relay.test",
                "--token",
                OPERATOR_TOKEN,
                "--allow-insecure-http",
                "--poll-interval",
                "0.01",
                "--output-dir",
                str(tmp_path / "cancel-report"),
                "cancel",
                navigation["command_id"],
            ]
        )
    worker.join(timeout=3)
    assert exit_code == 0
