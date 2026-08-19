from __future__ import annotations

import json
import threading
import time
import urllib.parse
from pathlib import Path
from typing import Any
from unittest.mock import patch

from communication_test.cloud.app import create_app
from communication_test.cloud.config import DeviceSettings, Settings as CloudSettings
from communication_test.orsus.agent import OrsusAgent, Settings as AgentSettings
from navigation_test.tests.test_end_to_end import (
    CompletedController,
    UrlopenResponse,
    call_asgi,
    ip_runner,
)
from startup_test.client import main


OPERATOR_TOKEN = "operator-token-000000000000000000000000"
DEVICE_TOKEN = "orsus-token-000000000000000000000000000"
DEVICE_ID = "ORSUS-GO2-GSM20260003"


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
        return RequestsResponse(call_asgi(self.app, method, path, dict(self.headers), body))

    def close(self) -> None:
        return None


def test_startup_demo_crosses_ecs_agent_and_orsus_without_mission(tmp_path: Path) -> None:
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
            navigation_state_path=tmp_path / "startup-job.json",
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
        raise AssertionError("agent did not receive the startup command")

    def urlopen(request: Any, timeout: float) -> UrlopenResponse:
        del timeout
        parsed = urllib.parse.urlsplit(request.full_url)
        path = urllib.parse.urlunsplit(("", "", parsed.path, parsed.query, ""))
        headers = {key: value for key, value in request.header_items()}
        return UrlopenResponse(call_asgi(app, request.method, path, headers, request.data))

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
            ]
        )
    device.join(timeout=3)

    assert exit_code == 0
    report_path = next((tmp_path / "reports").glob("*.json"))
    report = json.loads(report_path.read_text(encoding="utf-8"))
    result = report["command"]["result"]
    assert report["passed"] is True
    assert report["command"]["type"] == "STARTUP"
    assert result["status"] == "ready"
    assert result["mission_id"] is None
    assert result["target"] is None
    assert result["localization"]["status"] == "successful"
    assert result["localization"]["map"] == "airs1f_3"
    assert result["localization"]["pose"]["theta"] == -0.072
    startup_data = result["startup_report"]["robots"]["go2"]["data"]
    assert startup_data["localization"] == result["localization"]
    assert "startup_report" in result
    assert "navigation_report" not in result
