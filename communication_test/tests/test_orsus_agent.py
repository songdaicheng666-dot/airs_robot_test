from __future__ import annotations

import json
import subprocess
from typing import Any, Callable

import pytest
import requests

from communication_test.orsus.agent import (
    VERSION,
    IdentityMismatch,
    OrsusAgent,
    RelayError,
    Settings,
)


DEVICE_TOKEN = "orsus-token-000000000000000000000000000"


class FakeResponse:
    def __init__(self, payload: Any, status_code: int = 200) -> None:
        self.payload = payload
        self.status_code = status_code
        self.text = json.dumps(payload)

    def json(self) -> Any:
        if isinstance(self.payload, BaseException):
            raise self.payload
        return self.payload


class FakeSession:
    def __init__(self, handler: Callable[..., FakeResponse]) -> None:
        self.handler = handler
        self.headers: dict[str, str] = {}
        self.calls: list[dict[str, Any]] = []

    def request(self, method: str, url: str, **kwargs: Any) -> FakeResponse:
        call = {"method": method, "url": url, **kwargs}
        self.calls.append(call)
        return self.handler(method, url, **kwargs)

    def close(self) -> None:
        return None


def settings(**overrides: Any) -> Settings:
    values = {
        "relay_base_url": "http://120.24.74.70",
        "device_id": "ORSUS-GO2-GSM20260003",
        "device_token": DEVICE_TOKEN,
        "expected_sn": "GSM20260003",
        "heartbeat_seconds": 5,
        "poll_seconds": 25,
    }
    values.update(overrides)
    return Settings(**values)


def wrapped(data: dict[str, Any]) -> dict[str, Any]:
    return {"code": 0, "msg": "success", "data": data}


def local_handler(*, navigation_status: int = 200, sn: str = "GSM20260003"):
    def handler(method: str, url: str, **_kwargs: Any) -> FakeResponse:
        if url.endswith("/healthz"):
            return FakeResponse(wrapped({"status": "ok"}))
        if url.endswith("/systems/device"):
            return FakeResponse(wrapped({"model": "Orsus-mini", "sn": sn, "version": "v1.0.0"}))
        if url.endswith("/services/status"):
            return FakeResponse(wrapped({"motion": {"status": "stopped"}, "scan": {"status": "degraded"}}))
        if url.endswith("/nav/container/status"):
            return FakeResponse(wrapped({"running": False, "status": "exited"}))
        if url.endswith("/nav/navigation_status"):
            if navigation_status != 200:
                return FakeResponse({"detail": "navigation unavailable"}, navigation_status)
            return FakeResponse(wrapped({"status": "idle"}))
        raise AssertionError(f"unexpected local request: {method} {url}")

    return handler


def ip_runner(command: list[str], **_kwargs: Any) -> subprocess.CompletedProcess[str]:
    if "address" in command:
        output = [{"ifname": "eth3", "addr_info": [{"family": "inet", "local": "192.168.0.69"}]}]
    elif "route" in command:
        output = [
            {
                "dst": "120.24.74.70",
                "gateway": "192.168.0.1",
                "dev": "eth3",
                "prefsrc": "192.168.0.69",
            }
        ]
    else:
        raise AssertionError(command)
    return subprocess.CompletedProcess(command, 0, json.dumps(output), "")


def test_collects_complete_status_and_isolates_navigation_error() -> None:
    local = FakeSession(local_handler(navigation_status=500))
    cloud = FakeSession(lambda *_args, **_kwargs: FakeResponse({}))
    agent = OrsusAgent(
        settings(),
        cloud_session=cloud,
        local_session=local,
        command_runner=ip_runner,
    )

    agent.verify_identity()
    telemetry = agent.collect_telemetry()

    assert telemetry["device"]["sn"] == "GSM20260003"
    assert telemetry["services"]["motion"]["status"] == "stopped"
    assert telemetry["navigation"]["container"]["status"] == "exited"
    assert telemetry["navigation"]["status"] is None
    assert telemetry["network"]["route_to_ecs"] == {
        "destination": "120.24.74.70",
        "gateway": "192.168.0.1",
        "interface": "eth3",
        "source": "192.168.0.69",
    }
    assert telemetry["errors"] == [
        {"source": "navigation_status", "message": "POST /nav/navigation_status returned HTTP 500"}
    ]
    assert all("/start" not in call["url"] for call in local.calls)


def test_ping_command_reports_received_then_completed() -> None:
    state_updates: list[dict[str, Any]] = []

    def cloud_handler(method: str, url: str, **kwargs: Any) -> FakeResponse:
        assert method == "POST"
        assert "/commands/command-1/state" in url
        state_updates.append(kwargs["json"])
        return FakeResponse({"state": kwargs["json"]["state"]})

    agent = OrsusAgent(
        settings(),
        cloud_session=FakeSession(cloud_handler),
        local_session=FakeSession(local_handler()),
        command_runner=ip_runner,
    )
    agent.execute_command({"command_id": "command-1", "type": "PING", "payload": {"message": "hello"}})

    assert [update["state"] for update in state_updates] == ["RECEIVED", "COMPLETED"]
    assert state_updates[1]["result"] == {
        "message": "pong",
        "echo": {"message": "hello"},
        "agent_version": VERSION,
        "device_sn": "GSM20260003",
    }


def test_status_query_returns_fresh_telemetry() -> None:
    state_updates: list[dict[str, Any]] = []

    def cloud_handler(_method: str, _url: str, **kwargs: Any) -> FakeResponse:
        state_updates.append(kwargs["json"])
        return FakeResponse({"state": kwargs["json"]["state"]})

    agent = OrsusAgent(
        settings(),
        cloud_session=FakeSession(cloud_handler),
        local_session=FakeSession(local_handler()),
        command_runner=ip_runner,
    )
    agent.execute_command({"command_id": "command-2", "type": "STATUS_QUERY", "payload": {}})

    telemetry = state_updates[-1]["result"]["telemetry"]
    assert telemetry["sequence"] == 1
    assert telemetry["edge_core"] == {"healthy": True, "status": "ok"}


def test_identity_mismatch_is_fatal() -> None:
    agent = OrsusAgent(
        settings(),
        cloud_session=FakeSession(lambda *_args, **_kwargs: FakeResponse({})),
        local_session=FakeSession(local_handler(sn="OTHER")),
        command_runner=ip_runner,
    )
    with pytest.raises(IdentityMismatch, match="expected GSM20260003, got OTHER"):
        agent.verify_identity()


def test_cloud_error_does_not_expose_authorization_token() -> None:
    cloud = FakeSession(lambda *_args, **_kwargs: FakeResponse({"detail": "invalid bearer token"}, 401))
    agent = OrsusAgent(
        settings(),
        cloud_session=cloud,
        local_session=FakeSession(local_handler()),
        command_runner=ip_runner,
    )
    with pytest.raises(RelayError) as captured:
        agent.cloud_request("GET", "/v1/devices/test/commands/next")
    assert DEVICE_TOKEN not in str(captured.value)
    assert cloud.headers["Authorization"] == f"Bearer {DEVICE_TOKEN}"


def test_rejects_short_device_token() -> None:
    with pytest.raises(Exception, match="at least 32"):
        OrsusAgent(settings(device_token="too-short"))
