from __future__ import annotations

import ipaddress
import json
import logging
import math
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

from orsus_nav import AppConfig, DualRobotController, HttpSettings, RobotConfig


VERSION = "2.0.0"
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


def boolean_value(name: str, value: str) -> bool:
    normalized = value.strip().lower()
    if normalized in {"1", "true", "yes", "on"}:
        return True
    if normalized in {"0", "false", "no", "off"}:
        return False
    raise ConfigError(f"{name} must be a boolean value")


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
    robot_name: str = "go2"
    adapter_type: str = "go2"
    scene_name: str = "airs1f_3"
    bringup_mode: str = "localization"
    relocalization_mode: str = "sequential"
    navigation_state_path: Path = Path("/var/lib/orsus-ecs-agent/navigation-job.json")
    command_renewal_seconds: float = 10.0
    allow_insecure_http: bool = False

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
            robot_name=os.environ.get("ORSUS_ROBOT_NAME", "go2"),
            adapter_type=os.environ.get("ORSUS_ADAPTER_TYPE", "go2"),
            scene_name=os.environ.get("ORSUS_SCENE_NAME", "airs1f_3"),
            bringup_mode=os.environ.get("ORSUS_BRINGUP_MODE", "localization"),
            relocalization_mode=os.environ.get("ORSUS_RELOCALIZATION_MODE", "sequential"),
            navigation_state_path=Path(
                os.environ.get(
                    "ORSUS_NAVIGATION_STATE_PATH",
                    "/var/lib/orsus-ecs-agent/navigation-job.json",
                )
            ),
            command_renewal_seconds=positive_number(
                "RELAY_COMMAND_RENEWAL_SECONDS",
                os.environ.get("RELAY_COMMAND_RENEWAL_SECONDS", "10"),
                maximum=30,
            ),
            allow_insecure_http=boolean_value(
                "RELAY_ALLOW_INSECURE_HTTP",
                os.environ.get("RELAY_ALLOW_INSECURE_HTTP", "false"),
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
        if not self.robot_name or not self.adapter_type or not self.scene_name:
            raise ConfigError("ORSUS_ROBOT_NAME, ORSUS_ADAPTER_TYPE, and ORSUS_SCENE_NAME are required")
        if self.bringup_mode not in {"localization", "mapping", "navigation"}:
            raise ConfigError("ORSUS_BRINGUP_MODE is invalid")
        if self.relocalization_mode not in {"global", "origin", "sequential"}:
            raise ConfigError("ORSUS_RELOCALIZATION_MODE is invalid")
        if self.command_renewal_seconds <= 0 or self.command_renewal_seconds > 30:
            raise ConfigError("RELAY_COMMAND_RENEWAL_SECONDS must be greater than zero and no more than 30")


class OrsusAgent:
    def __init__(
        self,
        settings: Settings,
        *,
        cloud_session: requests.Session | None = None,
        local_session: requests.Session | None = None,
        command_runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
        resolver: Callable[[str], str] = socket.gethostbyname,
        controller_factory: Callable[..., DualRobotController] = DualRobotController,
    ) -> None:
        settings.validate()
        self.settings = settings
        self.cloud = cloud_session or requests.Session()
        self.local = local_session or requests.Session()
        self.command_runner = command_runner
        self.resolver = resolver
        self.controller_factory = controller_factory
        self.stop_event = threading.Event()
        self.collect_lock = threading.Lock()
        self.navigation_lock = threading.RLock()
        self.metrics_lock = threading.Lock()
        self.sequence = 0
        self.device: dict[str, Any] | None = None
        self.active_job: dict[str, Any] | None = None
        self.active_controller: DualRobotController | None = None
        self.navigation_thread: threading.Thread | None = None
        self.cloud_metrics: list[dict[str, Any]] = []
        self.cloud_success_count = 0
        self.cloud_failure_count = 0
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
        controller = self.active_controller
        if controller is not None:
            try:
                controller.cancel_active_operations(stop_navigation=True)
            except Exception as exc:
                LOG.error("navigation cleanup during agent shutdown failed: %s", exc)
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
        started = time.perf_counter()
        try:
            response = self.cloud.request(
                method,
                f"{self.settings.relay_base_url.rstrip('/')}{path}",
                json=payload,
                timeout=timeout,
            )
        except requests.RequestException as exc:
            if not long_poll:
                self._record_cloud_metric(method, path, started, False)
            raise RelayError(f"{method} {path} failed: {self.redact(exc)}") from exc
        if not long_poll:
            self._record_cloud_metric(method, path, started, 200 <= response.status_code < 300)
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

    def _record_cloud_metric(self, method: str, path: str, started: float, success: bool) -> None:
        sample = {
            "recorded_at": utc_now(),
            "method": method,
            "path": path.split("?", 1)[0],
            "rtt_ms": round((time.perf_counter() - started) * 1000, 3),
            "success": success,
        }
        with self.metrics_lock:
            if success:
                self.cloud_success_count += 1
            else:
                self.cloud_failure_count += 1
            self.cloud_metrics.append(sample)
            if len(self.cloud_metrics) > 4096:
                del self.cloud_metrics[: len(self.cloud_metrics) - 4096]

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

    def require_5g_route(self) -> dict[str, Any]:
        network = self.network_status()
        route = network.get("route_to_ecs")
        if not network.get("ipv4"):
            raise LocalApiError(
                f"configured 5G interface {self.settings.network_interface} has no IPv4 address"
            )
        if not isinstance(route, dict) or route.get("interface") != self.settings.network_interface:
            actual = route.get("interface") if isinstance(route, dict) else None
            raise LocalApiError(
                f"route to ECS does not use configured 5G interface "
                f"{self.settings.network_interface}; actual={actual or '<missing>'}"
            )
        return network

    def _load_job(self) -> dict[str, Any] | None:
        path = self.settings.navigation_state_path
        if not path.exists():
            return None
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise ConfigError(f"could not read navigation state {path}: {exc}") from exc
        if not isinstance(value, dict) or not isinstance(value.get("command_id"), str):
            raise ConfigError(f"invalid navigation state in {path}")
        return value

    def _save_job(self, job: dict[str, Any]) -> None:
        path = self.settings.navigation_state_path
        temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            temporary.write_text(
                json.dumps(job, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            os.chmod(temporary, 0o600)
            os.replace(temporary, path)
        except OSError as exc:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass
            raise ConfigError(f"could not persist navigation state {path}: {exc}") from exc

    def _job_metric_snapshot(self, started_at: str) -> dict[str, Any]:
        with self.metrics_lock:
            samples = [
                dict(sample)
                for sample in self.cloud_metrics
                if sample["recorded_at"] >= started_at
            ]
        values = sorted(float(sample["rtt_ms"]) for sample in samples if sample["success"])

        def percentile(percent: float) -> float | None:
            if not values:
                return None
            position = (len(values) - 1) * percent
            lower = math.floor(position)
            upper = math.ceil(position)
            if lower == upper:
                return round(values[lower], 3)
            result = values[lower] + (values[upper] - values[lower]) * (position - lower)
            return round(result, 3)

        exported_samples = samples[-128:]
        return {
            "sample_count": len(samples),
            "success_count": sum(bool(sample["success"]) for sample in samples),
            "failure_count": sum(not bool(sample["success"]) for sample in samples),
            "rtt_ms": {
                "min": round(min(values), 3) if values else None,
                "avg": round(sum(values) / len(values), 3) if values else None,
                "p50": percentile(0.50),
                "p95": percentile(0.95),
                "p99": percentile(0.99),
                "max": round(max(values), 3) if values else None,
            },
            "samples": exported_samples,
            "samples_truncated": len(samples) - len(exported_samples),
        }

    def calibrate_clock(self, count: int = 5) -> dict[str, Any]:
        samples: list[dict[str, float]] = []
        for _ in range(count):
            before_ns = time.time_ns()
            response = self.cloud_request("GET", "/v1/time")
            after_ns = time.time_ns()
            if response is None or not isinstance(response.get("unix_time_ns"), int):
                raise RelayError("relay time endpoint returned invalid data")
            midpoint_ns = (before_ns + after_ns) / 2
            rtt_ms = (after_ns - before_ns) / 1_000_000
            samples.append(
                {
                    "offset_ms": (response["unix_time_ns"] - midpoint_ns) / 1_000_000,
                    "uncertainty_ms": rtt_ms / 2,
                    "rtt_ms": rtt_ms,
                }
            )
        best = min(samples, key=lambda item: item["rtt_ms"])
        return {
            "offset_ms": round(best["offset_ms"], 3),
            "uncertainty_ms": round(best["uncertainty_ms"], 3),
            "sample_count": len(samples),
        }

    def _robot_config(self, target: dict[str, float]) -> tuple[AppConfig, RobotConfig]:
        mission = {"mode": "standard", "frame_id": "map", "target": dict(target)}
        robot = RobotConfig(
            name=self.settings.robot_name,
            enabled=True,
            base_url=self.settings.orsus_base_url.rstrip("/"),
            expected_sn=self.settings.expected_sn,
            adapter_type=self.settings.adapter_type,
            scene_name=self.settings.scene_name,
            bringup_mode=self.settings.bringup_mode,
            relocalization_mode=self.settings.relocalization_mode,
            mission=mission,
        )
        controller_state = self.settings.navigation_state_path.with_name("orsus-controller-state.json")
        http = HttpSettings(
            connect_timeout_seconds=self.settings.connect_timeout_seconds,
            read_timeout_seconds=max(self.settings.read_timeout_seconds, 30),
            long_operation_timeout_seconds=300,
            poll_interval_seconds=1,
            service_start_timeout_seconds=60,
            container_start_timeout_seconds=180,
            mission_timeout_seconds=0,
            read_retries=2,
            retry_backoff_seconds=0.5,
        )
        return AppConfig(http=http, robots={robot.name: robot}, state_file=controller_state), robot

    @staticmethod
    def _validated_target(payload: dict[str, Any]) -> dict[str, float]:
        if set(payload) != {"target"} or not isinstance(payload.get("target"), dict):
            raise ConfigError("NAVIGATE payload must contain only target")
        target = payload["target"]
        if set(target) != {"x", "y", "theta"}:
            raise ConfigError("navigation target must contain exactly x, y, and theta")
        values: dict[str, float] = {}
        for name in ("x", "y", "theta"):
            value = target[name]
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
                raise ConfigError(f"navigation target {name} must be a finite number")
            values[name] = float(value)
        return values

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
                "navigation": {
                    "container": container,
                    "status": navigation_status,
                    "profile": {
                        "robot_name": self.settings.robot_name,
                        "adapter_type": self.settings.adapter_type,
                        "scene_name": self.settings.scene_name,
                        "bringup_mode": self.settings.bringup_mode,
                        "relocalization_mode": self.settings.relocalization_mode,
                    },
                },
                "errors": errors[:16],
            }

    def post_state(
        self,
        command_id: str,
        state: str,
        *,
        result: dict[str, Any] | None = None,
        error: str | None = None,
        progress: dict[str, Any] | None = None,
        device_recorded_at: str | None = None,
    ) -> None:
        payload: dict[str, Any] = {"state": state, "result": result, "error": error}
        if progress is not None:
            payload["progress"] = progress
        if device_recorded_at is not None:
            payload["device_recorded_at"] = device_recorded_at
        self.cloud_request(
            "POST",
            f"/v1/devices/{self.settings.device_id}/commands/{command_id}/state",
            payload=payload,
        )

    def _create_controller(self, target: dict[str, float]) -> DualRobotController:
        config, robot = self._robot_config(target)
        return self.controller_factory(
            config,
            [robot],
            progress_callback=self._navigation_progress,
        )

    def _navigation_progress(self, _robot_name: str, event: dict[str, Any]) -> None:
        now = utc_now()
        with self.navigation_lock:
            job = self.active_job
            if job is None or job.get("status") not in {"received", "running"}:
                return
            job["status"] = "running"
            job["phase"] = event.get("phase", "navigation")
            job["phase_status"] = event.get("status", "running")
            if event.get("mission_id"):
                job["mission_id"] = str(event["mission_id"])
            job.setdefault("phase_events", []).append({**event, "recorded_at": now})
            job["updated_at"] = now
            self._save_job(job)
            command_id = str(job["command_id"])
            progress = self._job_progress(job)
        try:
            self.post_state(
                command_id,
                "RUNNING",
                progress=progress,
                device_recorded_at=now,
            )
        except RelayError as exc:
            LOG.warning("navigation progress upload failed: %s", exc)

    @staticmethod
    def _job_progress(job: dict[str, Any]) -> dict[str, Any]:
        return {
            "phase": job.get("phase", "received"),
            "phase_status": job.get("phase_status", job.get("status", "received")),
            "mission_id": job.get("mission_id"),
            "target": job.get("target"),
            "updated_at": job.get("updated_at"),
        }

    @staticmethod
    def _phase_durations(events: list[dict[str, Any]]) -> dict[str, float]:
        starts: dict[str, datetime] = {}
        durations: dict[str, float] = {}
        for event in events:
            try:
                instant = datetime.fromisoformat(str(event["recorded_at"]).replace("Z", "+00:00"))
            except (KeyError, TypeError, ValueError):
                continue
            phase = str(event.get("phase", "unknown"))
            status = str(event.get("status", ""))
            if status in {"started", "pending", "running", "recovering"}:
                starts.setdefault(phase, instant)
            elif status in {"completed", "failed", "cancelled"} and phase in starts:
                durations[phase] = round((instant - starts[phase]).total_seconds(), 3)
        return durations

    def _terminal_result(self, job: dict[str, Any], report: dict[str, Any]) -> dict[str, Any]:
        completed_at = utc_now()
        started = datetime.fromisoformat(str(job["started_at"]).replace("Z", "+00:00"))
        completed = datetime.fromisoformat(completed_at.replace("Z", "+00:00"))
        robot_result = report.get("robots", {}).get(self.settings.robot_name, {})
        data = robot_result.get("data") if isinstance(robot_result, dict) else None
        data = data if isinstance(data, dict) else {}
        result = {
            "status": data.get("status", "completed" if report.get("ok") else "failed"),
            "mission_id": data.get("mission_id") or job.get("mission_id"),
            "target": job["target"],
            "robot": {
                "name": self.settings.robot_name,
                "device_id": self.settings.device_id,
                "scene_name": self.settings.scene_name,
                "adapter_type": self.settings.adapter_type,
                "bringup_mode": self.settings.bringup_mode,
            },
            "timing": {
                "started_at": job["started_at"],
                "completed_at": completed_at,
                "total_seconds": round((completed - started).total_seconds(), 3),
                "phase_seconds": self._phase_durations(job.get("phase_events", [])),
            },
            "communication_timestamps": {
                "cloud_created_at": job.get("cloud_created_at"),
                "device_received_at": job.get("device_received_at"),
                "device_completed_at": completed_at,
            },
            "clock_calibration": job.get("clock_calibration"),
            "network_metrics": self._job_metric_snapshot(job["started_at"]),
            "navigation_report": report,
        }
        return self._bounded_terminal_result(result)

    @staticmethod
    def _bounded_terminal_result(result: dict[str, Any]) -> dict[str, Any]:
        if len(json.dumps(result, ensure_ascii=False, separators=(",", ":")).encode("utf-8")) <= 48 * 1024:
            return result
        metrics = result.get("network_metrics")
        if isinstance(metrics, dict) and isinstance(metrics.get("samples"), list):
            removed = max(0, len(metrics["samples"]) - 32)
            metrics["samples"] = metrics["samples"][-32:]
            metrics["samples_truncated"] = int(metrics.get("samples_truncated", 0)) + removed
        result["result_truncated"] = True
        if len(json.dumps(result, ensure_ascii=False, separators=(",", ":")).encode("utf-8")) <= 48 * 1024:
            return result

        compact_robots: dict[str, Any] = {}
        report = result.get("navigation_report")
        if isinstance(report, dict):
            for name, item in report.get("robots", {}).items():
                if not isinstance(item, dict):
                    continue
                data = item.get("data") if isinstance(item.get("data"), dict) else {}
                compact_robots[str(name)] = {
                    "ok": item.get("ok"),
                    "error": item.get("error"),
                    "status": data.get("status"),
                    "mission_id": data.get("mission_id"),
                }
            result["navigation_report"] = {
                "ok": report.get("ok"),
                "robots": compact_robots,
                "truncated": True,
            }
        return result

    def _ack_terminal_job(self, job: dict[str, Any]) -> None:
        self.post_state(
            str(job["command_id"]),
            str(job["terminal_state"]),
            result=job.get("result"),
            error=job.get("error"),
            progress=self._job_progress(job),
            device_recorded_at=str(job["terminal_recorded_at"]),
        )
        with self.navigation_lock:
            job["terminal_acknowledged"] = True
            job["updated_at"] = utc_now()
            self._save_job(job)
            if self.active_job is job:
                self.active_job = None
                self.active_controller = None

    def _navigation_worker(
        self,
        job: dict[str, Any],
        controller: DualRobotController,
        *,
        resume_mission_id: str | None = None,
    ) -> None:
        try:
            try:
                calibration = self.calibrate_clock()
            except RelayError as exc:
                calibration = {"error": str(exc)}
            with self.navigation_lock:
                job["clock_calibration"] = calibration
                self._save_job(job)

            try:
                report = (
                    controller.resume_mission(resume_mission_id)
                    if resume_mission_id is not None
                    else controller.run()
                )
            except Exception as exc:
                report = {
                    "ok": False,
                    "robots": {
                        self.settings.robot_name: {
                            "ok": False,
                            "error": str(exc),
                            "error_type": type(exc).__name__,
                        }
                    },
                }

            with self.navigation_lock:
                cancel_requested = bool(job.get("cancel_requested"))
            if cancel_requested:
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    with self.navigation_lock:
                        if job.get("cancel_finished"):
                            break
                    time.sleep(0.05)

            result = self._terminal_result(job, report)
            with self.navigation_lock:
                if job.get("cancel_requested"):
                    cancel_ok = bool(job.get("cancel_ok"))
                    terminal_state = "CANCELLED" if cancel_ok else "FAILED"
                    result["status"] = "cancelled" if cancel_ok else "cancel_failed"
                    result["cancellation"] = job.get("cancel_result")
                else:
                    terminal_state = "COMPLETED" if report.get("ok") and result["status"] == "completed" else "FAILED"
                job["terminal_state"] = terminal_state
                job["status"] = terminal_state.lower()
                job["phase"] = "finished"
                job["phase_status"] = job["status"]
                job["result"] = result
                job["error"] = None if terminal_state in {"COMPLETED", "CANCELLED"} else self._navigation_error(report)
                job["terminal_recorded_at"] = result["timing"]["completed_at"]
                job["terminal_acknowledged"] = False
                self._save_job(job)
            try:
                self._ack_terminal_job(job)
            except RelayError as exc:
                LOG.warning("navigation terminal state upload failed; will retry: %s", exc)
        finally:
            controller.close()

    @staticmethod
    def _navigation_error(report: dict[str, Any]) -> str:
        errors = []
        for result in report.get("robots", {}).values():
            if not result.get("ok"):
                errors.append(str(result.get("error") or result.get("data", {}).get("status") or "failed"))
        return "; ".join(errors)[:1024] or "navigation failed"

    def _begin_navigation(self, command: dict[str, Any], target: dict[str, float]) -> None:
        command_id = str(command["command_id"])
        received_at = utc_now()
        with self.navigation_lock:
            if self.active_job is not None:
                if self.active_job.get("command_id") == command_id:
                    if self.active_job.get("terminal_state"):
                        self._ack_terminal_job(self.active_job)
                    else:
                        self.post_state(
                            command_id,
                            "RUNNING",
                            progress=self._job_progress(self.active_job),
                            device_recorded_at=received_at,
                        )
                    return
                raise ConfigError(
                    f"navigation command {self.active_job['command_id']} is already active"
                )

        parsed = urlparse(self.settings.relay_base_url)
        if parsed.scheme != "https" and not self.settings.allow_insecure_http:
            raise ConfigError("navigation over insecure relay HTTP is disabled")
        self.require_5g_route()
        self.post_state(command_id, "RECEIVED", device_recorded_at=received_at)
        controller = self._create_controller(target)
        job = {
            "command_id": command_id,
            "client_request_id": command.get("client_request_id"),
            "cloud_created_at": command.get("created_at"),
            "device_received_at": received_at,
            "target": target,
            "status": "received",
            "phase": "received",
            "phase_status": "received",
            "mission_id": None,
            "started_at": received_at,
            "updated_at": received_at,
            "phase_events": [],
            "terminal_acknowledged": False,
        }
        with self.navigation_lock:
            self.active_job = job
            self.active_controller = controller
            self._save_job(job)
        thread = threading.Thread(
            target=self._navigation_worker,
            args=(job, controller),
            name=f"navigation-{command_id[:8]}",
            daemon=True,
        )
        self.navigation_thread = thread
        thread.start()

    def _cancel_navigation(self, command_id: str, payload: dict[str, Any]) -> None:
        target_id = payload.get("navigation_command_id")
        if set(payload) != {"navigation_command_id"} or not isinstance(target_id, str):
            raise ConfigError("CANCEL_NAVIGATION requires navigation_command_id")
        self.post_state(command_id, "RECEIVED", device_recorded_at=utc_now())
        with self.navigation_lock:
            job = self.active_job
            controller = self.active_controller
            if job is None or job.get("command_id") != target_id or controller is None:
                raise ConfigError(f"navigation command {target_id} is not active on this device")
            job["cancel_requested"] = True
            job["updated_at"] = utc_now()
            self._save_job(job)
        result = controller.cancel_active_operations(stop_navigation=True)
        with self.navigation_lock:
            job["cancel_result"] = result
            job["cancel_ok"] = bool(result.get("ok"))
            job["cancel_finished"] = True
            job["updated_at"] = utc_now()
            self._save_job(job)
        if result.get("ok"):
            self.post_state(
                command_id,
                "COMPLETED",
                result={"navigation_command_id": target_id, "cancellation": result},
                device_recorded_at=utc_now(),
            )
        else:
            self.post_state(
                command_id,
                "FAILED",
                result={"navigation_command_id": target_id, "cancellation": result},
                error="navigation stop was not fully confirmed",
                device_recorded_at=utc_now(),
            )

    def renew_active_navigation(self) -> None:
        with self.navigation_lock:
            job = self.active_job
            if job is None:
                return
            terminal = job.get("terminal_state")
            acknowledged = bool(job.get("terminal_acknowledged"))
            progress = self._job_progress(job)
        if terminal and not acknowledged:
            self._ack_terminal_job(job)
        elif not terminal:
            self.post_state(
                str(job["command_id"]),
                "RUNNING",
                progress=progress,
                device_recorded_at=utc_now(),
            )

    def navigation_lease_loop(self) -> None:
        while not self.stop_event.wait(self.settings.command_renewal_seconds):
            try:
                self.renew_active_navigation()
            except RelayError as exc:
                LOG.warning("navigation lease renewal failed: %s", exc)

    def recover_navigation(self) -> None:
        job = self._load_job()
        if job is None or job.get("terminal_acknowledged"):
            return
        with self.navigation_lock:
            self.active_job = job
        if job.get("terminal_state"):
            try:
                self._ack_terminal_job(job)
            except RelayError as exc:
                LOG.warning("terminal navigation recovery upload failed: %s", exc)
            return

        target = self._validated_target({"target": job.get("target")})
        controller = self._create_controller(target)
        with self.navigation_lock:
            self.active_controller = controller
        mission_id = job.get("mission_id")
        if not mission_id:
            cleanup = controller.cancel_active_operations(stop_navigation=True)
            result = self._terminal_result(job, {"ok": False, "robots": {}})
            result["status"] = "submission_unknown_after_restart"
            result["recovery_cleanup"] = cleanup
            with self.navigation_lock:
                job.update(
                    {
                        "terminal_state": "FAILED",
                        "status": "failed",
                        "phase": "recovery",
                        "phase_status": "failed",
                        "result": result,
                        "error": "agent restarted before a mission ID was safely persisted",
                        "terminal_recorded_at": result["timing"]["completed_at"],
                    }
                )
                self._save_job(job)
            controller.close()
            try:
                self._ack_terminal_job(job)
            except RelayError as exc:
                LOG.warning("failed recovery state upload failed: %s", exc)
            return

        thread = threading.Thread(
            target=self._navigation_worker,
            args=(job, controller),
            kwargs={"resume_mission_id": str(mission_id)},
            name=f"navigation-recovery-{str(job['command_id'])[:8]}",
            daemon=True,
        )
        self.navigation_thread = thread
        thread.start()

    def execute_command(self, command: dict[str, Any]) -> None:
        command_id = command.get("command_id")
        command_type = command.get("type")
        payload = command.get("payload", {})
        if not isinstance(command_id, str) or not command_id:
            raise RelayError("relay returned a command without command_id")
        if not isinstance(payload, dict):
            self.post_state(command_id, "FAILED", error="command payload must be an object")
            return

        if command_type == "NAVIGATE":
            try:
                target = self._validated_target(payload)
                self._begin_navigation(command, target)
            except (ConfigError, LocalApiError) as exc:
                self.post_state(
                    command_id,
                    "FAILED",
                    error=str(exc),
                    device_recorded_at=utc_now(),
                )
            return
        if command_type == "CANCEL_NAVIGATION":
            try:
                self._cancel_navigation(command_id, payload)
            except (ConfigError, LocalApiError) as exc:
                self.post_state(
                    command_id,
                    "FAILED",
                    error=str(exc),
                    device_recorded_at=utc_now(),
                )
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
        lease = threading.Thread(
            target=self.navigation_lease_loop,
            name="orsus-navigation-lease",
            daemon=True,
        )
        heartbeat.start()
        lease.start()
        self.recover_navigation()
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
