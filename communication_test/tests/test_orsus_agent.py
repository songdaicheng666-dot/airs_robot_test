from __future__ import annotations

import json
import subprocess
import threading
import time
from datetime import datetime, timezone
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


class FakeNavigationController:
    def __init__(self, _config: Any, selected: list[Any], *, progress_callback: Callable[..., None]):
        self.robot_name = selected[0].name
        self.progress_callback = progress_callback
        self.stop_event = threading.Event()
        self.closed = False
        self.resume_ids: list[str] = []

    def run(self) -> dict[str, Any]:
        self.progress_callback(self.robot_name, {"phase": "preflight", "status": "started"})
        self.progress_callback(self.robot_name, {"phase": "preflight", "status": "completed"})
        self.progress_callback(
            self.robot_name,
            {"phase": "mission_submission", "status": "completed", "mission_id": "mission-1"},
        )
        self.progress_callback(
            self.robot_name,
            {"phase": "mission", "status": "completed", "mission_id": "mission-1"},
        )
        return {
            "ok": True,
            "robots": {
                self.robot_name: {
                    "ok": True,
                    "data": {"success": True, "status": "completed", "mission_id": "mission-1"},
                }
            },
        }

    def resume_mission(self, mission_id: str) -> dict[str, Any]:
        self.resume_ids.append(mission_id)
        return self.run()

    def cancel_active_operations(self, *, stop_navigation: bool = False) -> dict[str, Any]:
        self.stop_event.set()
        return {"ok": stop_navigation, "missions": {}, "relocalizations": {}}

    def close(self) -> None:
        self.closed = True


class BlockingNavigationController(FakeNavigationController):
    def __init__(self, *args: Any, **kwargs: Any):
        super().__init__(*args, **kwargs)
        self.release = threading.Event()

    def run(self) -> dict[str, Any]:
        self.progress_callback(self.robot_name, {"phase": "mission", "status": "running", "mission_id": "m"})
        self.release.wait(2)
        return {
            "ok": False,
            "robots": {self.robot_name: {"ok": False, "error": "interrupted"}},
        }

    def cancel_active_operations(self, *, stop_navigation: bool = False) -> dict[str, Any]:
        self.stop_event.set()
        self.release.set()
        return {"ok": stop_navigation, "missions": {}, "relocalizations": {}}


def navigation_cloud_handler(state_updates: list[tuple[str, dict[str, Any]]]):
    def handler(method: str, url: str, **kwargs: Any) -> FakeResponse:
        if url.endswith("/v1/time"):
            return FakeResponse(
                {
                    "server_time": datetime.now(tz=timezone.utc).isoformat(),
                    "unix_time_ns": time.time_ns(),
                }
            )
        if "/state" in url:
            command_id = url.split("/commands/", 1)[1].split("/", 1)[0]
            state_updates.append((command_id, kwargs["json"]))
            return FakeResponse({"state": kwargs["json"]["state"]})
        raise AssertionError(f"unexpected cloud request: {method} {url}")

    return handler


def navigation_command(command_id: str = "nav-command") -> dict[str, Any]:
    return {
        "command_id": command_id,
        "client_request_id": "request-1",
        "created_at": datetime.now(tz=timezone.utc).isoformat(),
        "type": "NAVIGATE",
        "payload": {"target": {"x": 1.0, "y": 2.0, "theta": 0.5}},
    }


def test_navigation_runs_in_background_and_reports_completed(tmp_path: Any) -> None:
    updates: list[tuple[str, dict[str, Any]]] = []
    agent = OrsusAgent(
        settings(
            allow_insecure_http=True,
            navigation_state_path=tmp_path / "navigation.json",
        ),
        cloud_session=FakeSession(navigation_cloud_handler(updates)),
        local_session=FakeSession(local_handler()),
        command_runner=ip_runner,
        controller_factory=FakeNavigationController,
    )

    agent.execute_command(navigation_command())
    assert agent.navigation_thread is not None
    agent.navigation_thread.join(timeout=3)

    states = [payload["state"] for command_id, payload in updates if command_id == "nav-command"]
    assert states[0] == "RECEIVED"
    assert "RUNNING" in states
    assert states[-1] == "COMPLETED"
    result = updates[-1][1]["result"]
    assert result["status"] == "completed"
    assert result["mission_id"] == "mission-1"
    persisted = json.loads((tmp_path / "navigation.json").read_text(encoding="utf-8"))
    assert persisted["terminal_acknowledged"] is True


def test_terminal_result_is_bounded_without_losing_summary_fields() -> None:
    result = {
        "status": "completed",
        "mission_id": "mission-large",
        "target": {"x": 1.0, "y": 2.0, "theta": 0.5},
        "timing": {
            "total_seconds": 12.5,
            "phase_seconds": {"startup": 1.0, "relocalization": 2.0, "mission": 9.5},
        },
        "network_metrics": {
            "sample_count": 200,
            "success_count": 199,
            "failure_count": 1,
            "rtt_ms": {"min": 1.0, "avg": 2.0, "p50": 2.0, "p95": 3.0, "p99": 4.0},
            "samples": [
                {
                    "recorded_at": "2026-08-17T00:00:00Z",
                    "method": "POST",
                    "path": f"/v1/commands/{index}/state/" + ("x" * 512),
                    "rtt_ms": 2.0,
                    "success": True,
                }
                for index in range(200)
            ],
            "samples_truncated": 0,
        },
        "navigation_report": {
            "ok": True,
            "robots": {
                "go2": {
                    "ok": True,
                    "data": {
                        "status": "completed",
                        "mission_id": "mission-large",
                        "diagnostics": "x" * 100_000,
                    },
                }
            },
        },
    }

    bounded = OrsusAgent._bounded_terminal_result(result)

    encoded = json.dumps(bounded, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    assert len(encoded) <= 48 * 1024
    assert bounded["status"] == "completed"
    assert bounded["mission_id"] == "mission-large"
    assert bounded["target"] == {"x": 1.0, "y": 2.0, "theta": 0.5}
    assert bounded["timing"]["phase_seconds"]["mission"] == 9.5
    assert bounded["network_metrics"]["rtt_ms"]["p95"] == 3.0
    assert len(bounded["network_metrics"]["samples"]) == 32
    assert bounded["network_metrics"]["samples_truncated"] == 168
    assert bounded["navigation_report"] == {
        "ok": True,
        "robots": {
            "go2": {
                "ok": True,
                "error": None,
                "status": "completed",
                "mission_id": "mission-large",
            }
        },
        "truncated": True,
    }
    assert bounded["result_truncated"] is True


def test_navigation_rejects_insecure_http_and_wrong_route(tmp_path: Any) -> None:
    updates: list[tuple[str, dict[str, Any]]] = []
    agent = OrsusAgent(
        settings(navigation_state_path=tmp_path / "navigation.json"),
        cloud_session=FakeSession(navigation_cloud_handler(updates)),
        local_session=FakeSession(local_handler()),
        command_runner=ip_runner,
        controller_factory=FakeNavigationController,
    )
    agent.execute_command(navigation_command("insecure"))
    assert updates[-1][1]["state"] == "FAILED"
    assert "insecure" in updates[-1][1]["error"]

    def wrong_route(command: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
        result = ip_runner(command, **kwargs)
        if "route" in command:
            value = json.loads(result.stdout)
            value[0]["dev"] = "wlan0"
            return subprocess.CompletedProcess(command, 0, json.dumps(value), "")
        return result

    updates.clear()
    routed = OrsusAgent(
        settings(
            allow_insecure_http=True,
            navigation_state_path=tmp_path / "wrong-route.json",
        ),
        cloud_session=FakeSession(navigation_cloud_handler(updates)),
        local_session=FakeSession(local_handler()),
        command_runner=wrong_route,
        controller_factory=FakeNavigationController,
    )
    routed.execute_command(navigation_command("wrong-route"))
    assert updates[-1][1]["state"] == "FAILED"
    assert "does not use configured 5G interface" in updates[-1][1]["error"]


def test_cancel_navigation_stops_active_controller(tmp_path: Any) -> None:
    updates: list[tuple[str, dict[str, Any]]] = []
    agent = OrsusAgent(
        settings(
            allow_insecure_http=True,
            navigation_state_path=tmp_path / "navigation.json",
        ),
        cloud_session=FakeSession(navigation_cloud_handler(updates)),
        local_session=FakeSession(local_handler()),
        command_runner=ip_runner,
        controller_factory=BlockingNavigationController,
    )
    agent.execute_command(navigation_command())
    agent.execute_command(
        {
            "command_id": "cancel-command",
            "type": "CANCEL_NAVIGATION",
            "payload": {"navigation_command_id": "nav-command"},
        }
    )
    assert agent.navigation_thread is not None
    agent.navigation_thread.join(timeout=3)

    cancel_states = [payload["state"] for cid, payload in updates if cid == "cancel-command"]
    nav_states = [payload["state"] for cid, payload in updates if cid == "nav-command"]
    assert cancel_states == ["RECEIVED", "COMPLETED"]
    assert nav_states[-1] == "CANCELLED"


def test_recovery_resumes_known_mission_without_submission(tmp_path: Any) -> None:
    updates: list[tuple[str, dict[str, Any]]] = []
    state_path = tmp_path / "navigation.json"
    now = datetime.now(tz=timezone.utc).isoformat().replace("+00:00", "Z")
    state_path.write_text(
        json.dumps(
            {
                "command_id": "nav-command",
                "target": {"x": 1.0, "y": 2.0, "theta": 0.5},
                "status": "running",
                "phase": "mission",
                "phase_status": "running",
                "mission_id": "mission-existing",
                "started_at": now,
                "updated_at": now,
                "phase_events": [],
                "terminal_acknowledged": False,
            }
        ),
        encoding="utf-8",
    )
    controllers: list[FakeNavigationController] = []

    def factory(*args: Any, **kwargs: Any) -> FakeNavigationController:
        controller = FakeNavigationController(*args, **kwargs)
        controllers.append(controller)
        return controller

    agent = OrsusAgent(
        settings(
            allow_insecure_http=True,
            navigation_state_path=state_path,
        ),
        cloud_session=FakeSession(navigation_cloud_handler(updates)),
        local_session=FakeSession(local_handler()),
        command_runner=ip_runner,
        controller_factory=factory,
    )
    agent.recover_navigation()
    assert agent.navigation_thread is not None
    agent.navigation_thread.join(timeout=3)
    assert controllers[0].resume_ids == ["mission-existing"]
    assert updates[-1][1]["state"] == "COMPLETED"
