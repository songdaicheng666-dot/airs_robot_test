from __future__ import annotations

import sqlite3
import uuid
from datetime import datetime, timezone
from pathlib import Path

import pytest
from httpx import ASGITransport, AsyncClient

from communication_test.cloud.app import create_app
from communication_test.cloud.config import DeviceSettings, Settings, load_device_registry


OPERATOR_TOKEN = "operator-token-000000000000000000000000"
DEVICE_TOKEN = "device-token-00000000000000000000000000"
ORSUS_TOKEN = "orsus-token-000000000000000000000000000"
ORSUS_DEVICE_ID = "ORSUS-GO2-GSM20260003"


def make_app(database_path: Path):
    return create_app(
        Settings(
            database_path=database_path,
            operator_token=OPERATOR_TOKEN,
            device_token=DEVICE_TOKEN,
            online_threshold_seconds=15,
            command_lease_seconds=3,
            command_ttl_seconds=60,
            max_poll_seconds=2,
        )
    )


def make_multi_device_app(database_path: Path):
    return create_app(
        Settings(
            database_path=database_path,
            operator_token=OPERATOR_TOKEN,
            online_threshold_seconds=15,
            command_lease_seconds=3,
            command_ttl_seconds=60,
            max_poll_seconds=2,
            devices={
                "M4T-001": DeviceSettings("M4T-001", "m4t", DEVICE_TOKEN),
                ORSUS_DEVICE_ID: DeviceSettings(
                    ORSUS_DEVICE_ID,
                    "orsus",
                    ORSUS_TOKEN,
                    expected_sn="GSM20260003",
                ),
            },
        )
    )


def auth(token: str) -> dict[str, str]:
    return {"Authorization": f"Bearer {token}"}


def telemetry(sequence: int = 1) -> dict:
    return {
        "recorded_at": datetime.now(tz=timezone.utc).isoformat(),
        "sequence": sequence,
        "psdk_connected": True,
        "flight": {"valid": True, "status_code": 0, "status": "STOPPED", "display_mode_code": 6},
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
        "rtk": {"valid": True, "connected": True, "position_solution": 50},
        "battery": {"valid": True, "percentage": 87, "voltage_v": 51.2, "current_a": -1.4},
        "errors": [],
    }


def orsus_telemetry(sn: str = "GSM20260003", sequence: int = 1) -> dict:
    return {
        "recorded_at": datetime.now(tz=timezone.utc).isoformat(),
        "sequence": sequence,
        "platform": "orsus",
        "agent": {"version": "1.0.0"},
        "device": {"model": "Orsus-mini", "sn": sn, "version": "v1.0.0"},
        "edge_core": {"healthy": True, "status": "ok"},
        "network": {
            "interface": "eth3",
            "ipv4": "192.168.0.69",
            "route_to_ecs": {
                "destination": "120.24.74.70",
                "gateway": "192.168.0.1",
                "interface": "eth3",
                "source": "192.168.0.69",
            },
        },
        "services": {"motion": {"status": "stopped"}},
        "navigation": {"container": {"status": "exited"}, "status": None},
        "errors": [{"source": "navigation_status", "message": "HTTP 500"}],
    }


@pytest.fixture
def anyio_backend():
    return "asyncio"


async def submit(client: AsyncClient, request_id: str | None = None, command_type: str = "PING"):
    return await client.post(
        "/v1/devices/M4T-001/commands",
        headers=auth(OPERATOR_TOKEN),
        json={
            "client_request_id": request_id or str(uuid.uuid4()),
            "type": command_type,
            "payload": {"message": "test"} if command_type == "PING" else {},
        },
    )


@pytest.mark.anyio
async def test_authentication_and_unknown_device(tmp_path: Path) -> None:
    transport = ASGITransport(app=make_app(tmp_path / "relay.db"))
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        assert (await client.get("/health")).status_code == 200
        assert (await client.get("/v1/devices/M4T-001/status")).status_code == 401
        assert (await client.get("/v1/devices/M4T-001/status", headers=auth("x" * 32))).status_code == 401
        assert (await client.get("/v1/devices/OTHER/status", headers=auth(OPERATOR_TOKEN))).status_code == 404


@pytest.mark.anyio
async def test_command_lifecycle_and_idempotency(tmp_path: Path) -> None:
    transport = ASGITransport(app=make_app(tmp_path / "relay.db"))
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        request_id = str(uuid.uuid4())
        created = await submit(client, request_id)
        assert created.status_code == 201
        command_id = created.json()["command_id"]

        repeated = await submit(client, request_id)
        assert repeated.status_code == 200
        assert repeated.json()["command_id"] == command_id

        conflict = await client.post(
            "/v1/devices/M4T-001/commands",
            headers=auth(OPERATOR_TOKEN),
            json={"client_request_id": request_id, "type": "STATUS_QUERY", "payload": {}},
        )
        assert conflict.status_code == 409

        claimed = await client.get(
            "/v1/devices/M4T-001/commands/next?timeout_s=0", headers=auth(DEVICE_TOKEN)
        )
        assert claimed.status_code == 200
        assert claimed.json()["command_id"] == command_id
        assert claimed.json()["state"] == "DELIVERED"

        received = await client.post(
            f"/v1/devices/M4T-001/commands/{command_id}/state",
            headers=auth(DEVICE_TOKEN),
            json={"state": "RECEIVED"},
        )
        assert received.status_code == 200
        completed = await client.post(
            f"/v1/devices/M4T-001/commands/{command_id}/state",
            headers=auth(DEVICE_TOKEN),
            json={"state": "COMPLETED", "result": {"message": "pong"}},
        )
        assert completed.status_code == 200
        assert completed.json()["result"] == {"message": "pong"}

        queried = await client.get(f"/v1/commands/{command_id}", headers=auth(OPERATOR_TOKEN))
        assert queried.json()["state"] == "COMPLETED"
        assert (await client.get(
            "/v1/devices/M4T-001/commands/next?timeout_s=0", headers=auth(DEVICE_TOKEN)
        )).status_code == 204


@pytest.mark.anyio
async def test_expired_lease_is_redelivered(tmp_path: Path) -> None:
    database_path = tmp_path / "relay.db"
    transport = ASGITransport(app=make_app(database_path))
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        command_id = (await submit(client)).json()["command_id"]
        first = await client.get(
            "/v1/devices/M4T-001/commands/next?timeout_s=0", headers=auth(DEVICE_TOKEN)
        )
        assert first.json()["delivery_count"] == 1
        received = await client.post(
            f"/v1/devices/M4T-001/commands/{command_id}/state",
            headers=auth(DEVICE_TOKEN),
            json={"state": "RECEIVED"},
        )
        assert received.json()["state"] == "RECEIVED"
        with sqlite3.connect(database_path) as connection:
            connection.execute("UPDATE commands SET lease_until = 0 WHERE command_id = ?", (command_id,))
        second = await client.get(
            "/v1/devices/M4T-001/commands/next?timeout_s=0", headers=auth(DEVICE_TOKEN)
        )
        assert second.json()["command_id"] == command_id
        assert second.json()["state"] == "DELIVERED"
        assert second.json()["delivery_count"] == 2


@pytest.mark.anyio
async def test_telemetry_and_restart_persistence(tmp_path: Path) -> None:
    database_path = tmp_path / "relay.db"
    transport = ASGITransport(app=make_app(database_path))
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        posted = await client.post(
            "/v1/devices/M4T-001/telemetry", headers=auth(DEVICE_TOKEN), json=telemetry()
        )
        assert posted.status_code == 202
        command_id = (await submit(client, command_type="STATUS_QUERY")).json()["command_id"]

    restarted_transport = ASGITransport(app=make_app(database_path))
    async with AsyncClient(transport=restarted_transport, base_url="http://test") as restarted:
        status_response = await restarted.get("/v1/devices/M4T-001/status", headers=auth(OPERATOR_TOKEN))
        assert status_response.status_code == 200
        assert status_response.json()["online"] is True
        assert status_response.json()["telemetry"]["battery"]["percentage"] == 87
        command_response = await restarted.get(f"/v1/commands/{command_id}", headers=auth(OPERATOR_TOKEN))
        assert command_response.json()["state"] == "QUEUED"


@pytest.mark.anyio
async def test_rejects_invalid_telemetry(tmp_path: Path) -> None:
    transport = ASGITransport(app=make_app(tmp_path / "relay.db"))
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        value = telemetry()
        value["battery"]["percentage"] = 101
        response = await client.post(
            "/v1/devices/M4T-001/telemetry", headers=auth(DEVICE_TOKEN), json=value
        )
        assert response.status_code == 422


@pytest.mark.anyio
async def test_long_poll_timeout_returns_no_content(tmp_path: Path) -> None:
    transport = ASGITransport(app=make_app(tmp_path / "relay.db"))
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        response = await client.get(
            "/v1/devices/M4T-001/commands/next?timeout_s=1",
            headers=auth(DEVICE_TOKEN),
        )
        assert response.status_code == 204
        assert not response.content


@pytest.mark.anyio
async def test_multi_device_authentication_and_listing(tmp_path: Path) -> None:
    transport = ASGITransport(app=make_multi_device_app(tmp_path / "relay.db"))
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        listing = await client.get("/v1/devices", headers=auth(OPERATOR_TOKEN))
        assert listing.status_code == 200
        assert listing.json() == {
            "devices": [
                {
                    "device_id": "M4T-001",
                    "device_type": "m4t",
                    "online": False,
                    "last_seen_at": None,
                },
                {
                    "device_id": ORSUS_DEVICE_ID,
                    "device_type": "orsus",
                    "online": False,
                    "last_seen_at": None,
                },
            ]
        }
        wrong_token = await client.get(
            "/v1/devices/M4T-001/commands/next?timeout_s=0",
            headers=auth(ORSUS_TOKEN),
        )
        assert wrong_token.status_code == 401
        unknown = await client.get(
            "/v1/devices/UNKNOWN/commands/next?timeout_s=0",
            headers=auth(ORSUS_TOKEN),
        )
        assert unknown.status_code == 404


@pytest.mark.anyio
async def test_orsus_telemetry_type_and_sn_validation(tmp_path: Path) -> None:
    transport = ASGITransport(app=make_multi_device_app(tmp_path / "relay.db"))
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        posted = await client.post(
            f"/v1/devices/{ORSUS_DEVICE_ID}/telemetry",
            headers=auth(ORSUS_TOKEN),
            json=orsus_telemetry(),
        )
        assert posted.status_code == 202

        status_response = await client.get(
            f"/v1/devices/{ORSUS_DEVICE_ID}/status",
            headers=auth(OPERATOR_TOKEN),
        )
        assert status_response.status_code == 200
        assert status_response.json()["device_type"] == "orsus"
        assert status_response.json()["telemetry"]["device"]["sn"] == "GSM20260003"

        wrong_sn = await client.post(
            f"/v1/devices/{ORSUS_DEVICE_ID}/telemetry",
            headers=auth(ORSUS_TOKEN),
            json=orsus_telemetry(sn="WRONG"),
        )
        assert wrong_sn.status_code == 422
        assert wrong_sn.json()["detail"] == "device SN does not match configured identity"

        wrong_type = await client.post(
            f"/v1/devices/{ORSUS_DEVICE_ID}/telemetry",
            headers=auth(ORSUS_TOKEN),
            json=telemetry(),
        )
        assert wrong_type.status_code == 422
        assert wrong_type.json()["detail"] == "expected Orsus telemetry"


def test_load_device_registry(tmp_path: Path) -> None:
    registry = tmp_path / "devices.json"
    registry.write_text(
        """{
          "M4T-001": {"kind": "m4t", "token": "device-token-00000000000000000000000000"},
          "ORSUS-GO2-GSM20260003": {
            "kind": "orsus",
            "token": "orsus-token-000000000000000000000000000",
            "expected_sn": "GSM20260003"
          }
        }""",
        encoding="utf-8",
    )
    devices = load_device_registry(registry)
    assert set(devices) == {"M4T-001", ORSUS_DEVICE_ID}
    assert devices[ORSUS_DEVICE_ID].expected_sn == "GSM20260003"
