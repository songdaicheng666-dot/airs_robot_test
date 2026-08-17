from __future__ import annotations

import ipaddress
import json
import logging
import os
import re
import signal
import socket
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable
from urllib.parse import urlparse

# Deployment may provide distro-compatible pure Python dependencies locally when
# the Orsus system package database cannot safely install python3-requests.
vendor_path = Path(__file__).resolve().parent / "vendor"
if vendor_path.is_dir():
    sys.path.insert(0, str(vendor_path))

import requests


VERSION = "1.0.0"
LOG = logging.getLogger("orsus_ecs_agent")
DEVICE_ID_PATTERN = re.compile(r"^[A-Za-z0-9._-]{1,64}$")


class AgentError(RuntimeError):
    pass


class ConfigError(AgentError):
    pass


class RelayError(AgentError):
    pass


class LocalApiError(AgentError):
    pass


class IdentityMismatch(AgentError):
    pass


def utc_now() -> str:
    return datetime.now(tz=timezone.utc).isoformat().replace("+00:00", "Z")


def positive_number(name: str, value: str, *, maximum: float) -> float:
    try:
        parsed = float(value)
    except ValueError as exc:
        raise ConfigError(f"{name} must be a number") from exc
    if parsed <= 0 or parsed > maximum:
        raise ConfigError(f"{name} must be greater than zero and no more than {maximum:g}")
    return parsed


def bounded_integer(name: str, value: str, *, minimum: int, maximum: int) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise ConfigError(f"{name} must be an integer") from exc
    if parsed < minimum or parsed > maximum:
        raise ConfigError(f"{name} must be between {minimum} and {maximum}")
    return parsed


@dataclass(frozen=True)
class Settings:
    relay_base_url: str
    device_id: str
    device_token: str
    expected_sn: str
    orsus_base_url: str = "http://127.0.0.1:8898"
    network_interface: str = "eth3"
    heartbeat_seconds: float = 5.0
    poll_seconds: int = 25
    connect_timeout_seconds: float = 3.0
    read_timeout_seconds: float = 8.0

    @classmethod
    def from_env(cls) -> "Settings":
        settings = cls(
            relay_base_url=os.environ.get("RELAY_BASE_URL", "http://120.24.74.70"),
            device_id=os.environ.get("RELAY_DEVICE_ID", "ORSUS-GO2-GSM20260003"),
            device_token=os.environ.get("RELAY_DEVICE_TOKEN", ""),
            expected_sn=os.environ.get("ORSUS_EXPECTED_SN", "GSM20260003"),
            orsus_base_url=os.environ.get("ORSUS_BASE_URL", "http://127.0.0.1:8898"),
            network_interface=os.environ.get("ORSUS_NETWORK_INTERFACE", "eth3"),
            heartbeat_seconds=positive_number(
                "RELAY_HEARTBEAT_SECONDS",
                os.environ.get("RELAY_HEARTBEAT_SECONDS", "5"),
                maximum=300,
            ),
            poll_seconds=bounded_integer(
                "RELAY_POLL_SECONDS",
                os.environ.get("RELAY_POLL_SECONDS", "25"),
                minimum=1,
                maximum=30,
            ),
            connect_timeout_seconds=positive_number(
                "RELAY_CONNECT_TIMEOUT_SECONDS",
                os.environ.get("RELAY_CONNECT_TIMEOUT_SECONDS", "3"),
                maximum=60,
            ),
            read_timeout_seconds=positive_number(
                "RELAY_READ_TIMEOUT_SECONDS",
                os.environ.get("RELAY_READ_TIMEOUT_SECONDS", "8"),
                maximum=120,
            ),
        )
        settings.validate()
        return settings

    def validate(self) -> None:
        for name, value in (
            ("RELAY_BASE_URL", self.relay_base_url),
            ("ORSUS_BASE_URL", self.orsus_base_url),
        ):
            parsed = urlparse(value)
            if parsed.scheme not in {"http", "https"} or not parsed.hostname:
                raise ConfigError(f"{name} must be an absolute HTTP or HTTPS URL")
        if not DEVICE_ID_PATTERN.fullmatch(self.device_id):
            raise ConfigError("RELAY_DEVICE_ID contains invalid characters")
        if len(self.device_token) < 32:
            raise ConfigError("RELAY_DEVICE_TOKEN must contain at least 32 characters")
        if not self.expected_sn or len(self.expected_sn) > 64:
            raise ConfigError("ORSUS_EXPECTED_SN must contain between 1 and 64 characters")
        if not self.network_interface or len(self.network_interface) > 32:
            raise ConfigError("ORSUS_NETWORK_INTERFACE must contain between 1 and 32 characters")
        if self.poll_seconds < 1 or self.poll_seconds > 30:
            raise ConfigError("RELAY_POLL_SECONDS must be between 1 and 30")
        if self.heartbeat_seconds <= 0 or self.heartbeat_seconds > 300:
            raise ConfigError("RELAY_HEARTBEAT_SECONDS must be greater than zero and no more than 300")
        if self.connect_timeout_seconds <= 0 or self.read_timeout_seconds <= 0:
            raise ConfigError("HTTP timeouts must be positive")


class OrsusAgent:
    def __init__(
        self,
        settings: Settings,
        *,
        cloud_session: requests.Session | None = None,
        local_session: requests.Session | None = None,
        command_runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
        resolver: Callable[[str], str] = socket.gethostbyname,
    ) -> None:
        settings.validate()
        self.settings = settings
        self.cloud = cloud_session or requests.Session()
        self.local = local_session or requests.Session()
        self.command_runner = command_runner
        self.resolver = resolver
        self.stop_event = threading.Event()
        self.collect_lock = threading.Lock()
        self.sequence = 0
        self.device: dict[str, Any] | None = None
        self.cloud.headers.update(
            {
                "Authorization": f"Bearer {settings.device_token}",
                "Accept": "application/json",
                "Content-Type": "application/json",
                "User-Agent": f"orsus-ecs-agent/{VERSION}",
            }
        )
        self.local.headers.update(
            {
                "Accept": "application/json",
                "Content-Type": "application/json",
                "User-Agent": f"orsus-ecs-agent/{VERSION}",
            }
        )

    def close(self) -> None:
        self.stop_event.set()
        self.cloud.close()
        self.local.close()

    def redact(self, value: Any) -> str:
        return str(value).replace(self.settings.device_token, "<redacted>")

    @property
    def cloud_timeout(self) -> tuple[float, float]:
        return (self.settings.connect_timeout_seconds, self.settings.read_timeout_seconds)

    @property
    def local_timeout(self) -> tuple[float, float]:
        return (self.settings.connect_timeout_seconds, self.settings.read_timeout_seconds)

    def cloud_request(
        self,
        method: str,
        path: str,
        *,
        payload: dict[str, Any] | None = None,
        long_poll: bool = False,
    ) -> dict[str, Any] | None:
        timeout = self.cloud_timeout
        if long_poll:
            timeout = (
                self.settings.connect_timeout_seconds,
                self.settings.poll_seconds + self.settings.read_timeout_seconds,
            )
        try:
            response = self.cloud.request(
                method,
                f"{self.settings.relay_base_url.rstrip('/')}{path}",
                json=payload,
                timeout=timeout,
            )
        except requests.RequestException as exc:
            raise RelayError(f"{method} {path} failed: {self.redact(exc)}") from exc
        if response.status_code == 204:
            return None
        if not 200 <= response.status_code < 300:
            detail = self.redact(response.text[:512].replace("\n", " "))
            raise RelayError(f"{method} {path} returned HTTP {response.status_code}: {detail}")
        try:
            value = response.json()
        except ValueError as exc:
            raise RelayError(f"{method} {path} returned invalid JSON") from exc
        if not isinstance(value, dict):
            raise RelayError(f"{method} {path} returned a non-object response")
        return value

    def local_request(self, method: str, path: str, *, api: bool = True) -> dict[str, Any]:
        prefix = "/v1/api" if api else ""
        try:
            response = self.local.request(
                method,
                f"{self.settings.orsus_base_url.rstrip('/')}{prefix}{path}",
                timeout=self.local_timeout,
            )
        except requests.RequestException as exc:
            raise LocalApiError(f"{method} {path} failed: {exc}") from exc
        if not 200 <= response.status_code < 300:
            raise LocalApiError(f"{method} {path} returned HTTP {response.status_code}")
        try:
            wrapper = response.json()
        except ValueError as exc:
            raise LocalApiError(f"{method} {path} returned invalid JSON") from exc
        if not isinstance(wrapper, dict) or wrapper.get("code") != 0:
            raise LocalApiError(f"{method} {path} returned an unsuccessful response")
        data = wrapper.get("data")
        if not isinstance(data, dict):
            raise LocalApiError(f"{method} {path} returned invalid data")
        return data

    def verify_identity(self) -> dict[str, Any]:
        device = self.local_request("GET", "/systems/device")
        actual_sn = str(device.get("sn", ""))
        if actual_sn != self.settings.expected_sn:
            raise IdentityMismatch(
                f"Orsus SN mismatch: expected {self.settings.expected_sn}, got {actual_sn or '<missing>'}"
            )
        self.device = device
        return device

    def wait_for_identity(self) -> None:
        retry_seconds = 1.0
        while not self.stop_event.is_set():
            try:
                device = self.verify_identity()
                LOG.info(
                    "verified Orsus identity: model=%s sn=%s version=%s",
                    device.get("model"),
                    device.get("sn"),
                    device.get("version"),
                )
                return
            except IdentityMismatch:
                raise
            except LocalApiError as exc:
                LOG.warning("waiting for Orsus Edge Core: %s; retrying in %.0fs", exc, retry_seconds)
                self.stop_event.wait(retry_seconds)
                retry_seconds = min(retry_seconds * 2, 30)

    @staticmethod
    def error(source: str, exc: BaseException) -> dict[str, str]:
        message = str(exc).replace("\n", " ")[:1024] or exc.__class__.__name__
        return {"source": source[:64], "message": message}

    def _run_ip_json(self, arguments: list[str]) -> Any:
        try:
            result = self.command_runner(
                ["ip", "-j", *arguments],
                check=True,
                capture_output=True,
                text=True,
                timeout=3,
            )
            return json.loads(result.stdout)
        except (OSError, subprocess.SubprocessError, json.JSONDecodeError) as exc:
            raise LocalApiError(f"ip command failed: {exc}") from exc

    def network_status(self) -> dict[str, Any]:
        interface = self.settings.network_interface
        addresses = self._run_ip_json(["-4", "address", "show", "dev", interface])
        ipv4 = None
        if isinstance(addresses, list) and addresses:
            for item in addresses[0].get("addr_info", []):
                if item.get("family") == "inet" and item.get("local"):
                    ipv4 = str(item["local"])
                    break

        relay_host = urlparse(self.settings.relay_base_url).hostname or ""
        try:
            destination = str(ipaddress.ip_address(relay_host))
        except ValueError:
            try:
                destination = self.resolver(relay_host)
            except OSError as exc:
                raise LocalApiError(f"could not resolve relay host {relay_host}: {exc}") from exc
        routes = self._run_ip_json(["-4", "route", "get", destination])
        route = None
        if isinstance(routes, list) and routes:
            item = routes[0]
            route = {
                "destination": destination,
                "gateway": item.get("gateway"),
                "interface": str(item.get("dev", "")),
                "source": item.get("prefsrc") or item.get("src"),
            }
        return {"interface": interface, "ipv4": ipv4, "route_to_ecs": route}

    def collect_telemetry(self) -> dict[str, Any]:
        with self.collect_lock:
            self.sequence += 1
            errors: list[dict[str, str]] = []

            try:
                health = self.local_request("GET", "/healthz", api=False)
                edge_core = {"healthy": health.get("status") == "ok", "status": health.get("status")}
            except LocalApiError as exc:
                edge_core = {"healthy": False, "status": None}
                errors.append(self.error("edge_core_health", exc))

            try:
                device = self.local_request("GET", "/systems/device")
                if str(device.get("sn", "")) != self.settings.expected_sn:
                    raise IdentityMismatch("device SN changed while the agent was running")
                self.device = device
            except (LocalApiError, IdentityMismatch) as exc:
                device = self.device or {
                    "model": "unknown",
                    "sn": self.settings.expected_sn,
                    "version": "unknown",
                }
                errors.append(self.error("device", exc))

            try:
                services = self.local_request("GET", "/services/status")
            except LocalApiError as exc:
                services = {}
                errors.append(self.error("services", exc))

            try:
                container = self.local_request("GET", "/nav/container/status")
            except LocalApiError as exc:
                container = None
                errors.append(self.error("navigation_container", exc))

            try:
                navigation_status = self.local_request("POST", "/nav/navigation_status")
            except LocalApiError as exc:
                navigation_status = None
                errors.append(self.error("navigation_status", exc))

            try:
                network = self.network_status()
            except LocalApiError as exc:
                network = {
                    "interface": self.settings.network_interface,
                    "ipv4": None,
                    "route_to_ecs": None,
                }
                errors.append(self.error("network", exc))

            return {
                "recorded_at": utc_now(),
                "sequence": self.sequence,
                "platform": "orsus",
                "agent": {"version": VERSION},
                "device": {
                    "model": str(device.get("model", "unknown")),
                    "sn": str(device.get("sn", self.settings.expected_sn)),
                    "version": str(device.get("version", "unknown")),
                },
                "edge_core": edge_core,
                "network": network,
                "services": services,
                "navigation": {"container": container, "status": navigation_status},
                "errors": errors[:16],
            }

    def post_state(
        self,
        command_id: str,
        state: str,
        *,
        result: dict[str, Any] | None = None,
        error: str | None = None,
    ) -> None:
        self.cloud_request(
            "POST",
            f"/v1/devices/{self.settings.device_id}/commands/{command_id}/state",
            payload={"state": state, "result": result, "error": error},
        )

    def execute_command(self, command: dict[str, Any]) -> None:
        command_id = command.get("command_id")
        command_type = command.get("type")
        payload = command.get("payload", {})
        if not isinstance(command_id, str) or not command_id:
            raise RelayError("relay returned a command without command_id")
        if not isinstance(payload, dict):
            self.post_state(command_id, "FAILED", error="command payload must be an object")
            return

        self.post_state(command_id, "RECEIVED")
        if command_type == "PING":
            result = {
                "message": "pong",
                "echo": payload,
                "agent_version": VERSION,
                "device_sn": self.settings.expected_sn,
            }
        elif command_type == "STATUS_QUERY":
            result = {"telemetry": self.collect_telemetry()}
        else:
            self.post_state(command_id, "FAILED", error="unsupported command type")
            return
        self.post_state(command_id, "COMPLETED", result=result)
        LOG.info("completed %s command %s", command_type, command_id)

    def heartbeat_loop(self) -> None:
        retry_seconds = self.settings.heartbeat_seconds
        while not self.stop_event.is_set():
            try:
                telemetry = self.collect_telemetry()
                self.cloud_request(
                    "POST",
                    f"/v1/devices/{self.settings.device_id}/telemetry",
                    payload=telemetry,
                )
                retry_seconds = self.settings.heartbeat_seconds
            except (LocalApiError, RelayError) as exc:
                LOG.warning("telemetry upload failed: %s; retrying in %.0fs", exc, retry_seconds)
                retry_seconds = min(max(retry_seconds * 2, 1), 30)
            self.stop_event.wait(retry_seconds)

    def run(self) -> None:
        self.wait_for_identity()
        if self.stop_event.is_set():
            return
        heartbeat = threading.Thread(target=self.heartbeat_loop, name="orsus-heartbeat", daemon=True)
        heartbeat.start()
        retry_seconds = 1.0
        while not self.stop_event.is_set():
            try:
                command = self.cloud_request(
                    "GET",
                    f"/v1/devices/{self.settings.device_id}/commands/next"
                    f"?timeout_s={self.settings.poll_seconds}",
                    long_poll=True,
                )
                retry_seconds = 1.0
                if command is not None:
                    self.execute_command(command)
            except RelayError as exc:
                LOG.warning("command poll failed: %s; retrying in %.0fs", exc, retry_seconds)
                self.stop_event.wait(retry_seconds)
                retry_seconds = min(retry_seconds * 2, 30)


def main() -> int:
    logging.basicConfig(
        level=os.environ.get("LOG_LEVEL", "INFO").upper(),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    try:
        settings = Settings.from_env()
        agent = OrsusAgent(settings)
    except ConfigError as exc:
        LOG.error("configuration error: %s", exc)
        return 2

    def stop(_signal_number: int, _frame: Any) -> None:
        LOG.info("stopping Orsus ECS agent")
        agent.stop_event.set()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    try:
        agent.run()
    except IdentityMismatch as exc:
        LOG.error("identity verification failed: %s", exc)
        return 3
    finally:
        agent.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
