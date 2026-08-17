from __future__ import annotations

import asyncio
import json
import subprocess
import threading
import time
import urllib.parse
from pathlib import Path
from typing import Any
from unittest.mock import patch

from httpx import ASGITransport, AsyncClient

from communication_test.cloud.app import create_app
from communication_test.cloud.config import DeviceSettings, Settings as CloudSettings
from communication_test.orsus.agent import OrsusAgent, Settings as AgentSettings
from navigation_test.client import main


OPERATOR_TOKEN = "operator-token-000000000000000000000000"
DEVICE_TOKEN = "orsus-token-000000000000000000000000000"
DEVICE_ID = "ORSUS-GO2-GSM20260003"


def call_asgi(app: Any, method: str, url: str, headers: dict[str, str], body: bytes | None):
    async def invoke():
        transport = ASGITransport(app=app)
        async with AsyncClient(transport=transport, base_url="http://relay.test") as client:
            return await client.request(method, url, headers=headers, content=body)

    return asyncio.run(invoke())


class RequestsResponse:
    def __init__(self, response: Any):
        self.status_code = response.status_code
        self.text = response.text
        self._response = response

    def json(self) -> Any:
        return self._response.json()


class AsgiRequestsSession:
    def __init__(self, app: Any):
        self.app = app
        self.headers: dict[str, str] = {}

    def request(self, method: str, url: str, **kwargs: Any) -> RequestsResponse:
        parsed = urllib.parse.urlsplit(url)
        path = urllib.parse.urlunsplit(("", "", parsed.path, parsed.query, ""))
        body = None
        if kwargs.get("json") is not None:
            body = json.dumps(kwargs["json"]).encode("utf-8")
        response = call_asgi(self.app, method, path, dict(self.headers), body)
        return RequestsResponse(response)

    def close(self) -> None:
        return None


class UrlopenResponse:
    def __init__(self, response: Any):
        self.status = response.status_code
        self._body = response.content

    def read(self) -> bytes:
        return self._body

    def __enter__(self) -> "UrlopenResponse":
        return self

    def __exit__(self, *_args: Any) -> None:
        return None


class CompletedController:
    def __init__(self, _config: Any, selected: list[Any], *, progress_callback: Any):
        self.name = selected[0].name
        self.progress = progress_callback
        self.stop_event = threading.Event()

    def run(self) -> dict[str, Any]:
        for event in (
            {"phase": "preflight", "status": "started"},
            {"phase": "preflight", "status": "completed"},
            {"phase": "relocalization", "status": "started"},
            {"phase": "relocalization", "status": "completed"},
            {"phase": "mission_submission", "status": "completed", "mission_id": "mission-e2e"},
            {"phase": "mission", "status": "completed", "mission_id": "mission-e2e"},
        ):
            self.progress(self.name, event)
        return {
            "ok": True,
            "robots": {
                self.name: {
                    "ok": True,
                    "data": {"success": True, "status": "completed", "mission_id": "mission-e2e"},
                }
            },
        }

    def resume_mission(self, _mission_id: str) -> dict[str, Any]:
        return self.run()

    def cancel_active_operations(self, *, stop_navigation: bool = False) -> dict[str, Any]:
        return {"ok": stop_navigation, "missions": {}, "relocalizations": {}}

    def close(self) -> None:
        return None


def ip_runner(command: list[str], **_kwargs: Any) -> subprocess.CompletedProcess[str]:
    if "address" in command:
        output = [{"addr_info": [{"family": "inet", "local": "192.168.0.69"}]}]
    else:
        output = [{"dev": "eth3", "gateway": "192.168.0.1", "prefsrc": "192.168.0.69"}]
    return subprocess.CompletedProcess(command, 0, json.dumps(output), "")


def test_pc_to_ecs_to_agent_to_orsus_and_back(tmp_path: Path) -> None:
    app = create_app(
        CloudSettings(
            database_path=tmp_path / "relay.db",
            operator_token=OPERATOR_TOKEN,
            command_lease_seconds=3,
            command_ttl_seconds=60,
            max_poll_seconds=2,
            allow_insecure_navigation=True,
            devices={
                DEVICE_ID: DeviceSettings(
                    DEVICE_ID,
                    "orsus",
                    DEVICE_TOKEN,
                    expected_sn="GSM20260003",
                )
            },
        )
    )
    agent = OrsusAgent(
        AgentSettings(
            relay_base_url="http://relay.test",
            device_id=DEVICE_ID,
            device_token=DEVICE_TOKEN,
            expected_sn="GSM20260003",
            allow_insecure_http=True,
            navigation_state_path=tmp_path / "navigation-job.json",
        ),
        cloud_session=AsgiRequestsSession(app),
        local_session=AsgiRequestsSession(app),
        command_runner=ip_runner,
        resolver=lambda _host: "120.24.74.70",
        controller_factory=CompletedController,
    )

    def device_loop() -> None:
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            command = agent.cloud_request(
                "GET",
                f"/v1/devices/{DEVICE_ID}/commands/next?timeout_s=0",
                long_poll=True,
            )
            if command is not None:
                agent.execute_command(command)
                if agent.navigation_thread is not None:
                    agent.navigation_thread.join(timeout=3)
                return
            time.sleep(0.01)
        raise AssertionError("agent did not receive the navigation command")

    def urlopen(request: Any, timeout: float) -> UrlopenResponse:
        del timeout
        parsed = urllib.parse.urlsplit(request.full_url)
        path = urllib.parse.urlunsplit(("", "", parsed.path, parsed.query, ""))
        headers = {key: value for key, value in request.header_items()}
        response = call_asgi(app, request.method, path, headers, request.data)
        return UrlopenResponse(response)

    device = threading.Thread(target=device_loop, daemon=True)
    device.start()
    with patch("navigation_test.client.urllib.request.urlopen", side_effect=urlopen):
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
                str(tmp_path / "reports"),
                "run",
                "--device-id",
                DEVICE_ID,
                "--x",
                "1",
                "--y",
                "2",
                "--theta",
                "0.5",
            ]
        )
    device.join(timeout=3)

    assert exit_code == 0
    report_path = next((tmp_path / "reports").glob("*.json"))
    report = json.loads(report_path.read_text(encoding="utf-8"))
    assert report["passed"] is True
    assert report["command"]["result"]["mission_id"] == "mission-e2e"
    assert report["command"]["state"] == "COMPLETED"
