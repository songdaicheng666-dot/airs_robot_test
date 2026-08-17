#!/usr/bin/env python3
"""Dual-Orsus navigation controller using the Edge Core HTTP API."""

from __future__ import annotations

import argparse
import copy
import json
import logging
import os
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, replace
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping, Optional
from urllib.parse import urlparse

import requests
import yaml


LOG = logging.getLogger("orsus_nav")
TERMINAL_MISSION_STATES = {"completed", "failed", "cancelled"}
ACTIVE_MISSION_STATES = {"pending", "running", "stopping", "paused"}
RELOCALIZATION_MODES = {"global", "origin", "sequential"}
MISSION_MODES = {"standard", "direct", "route", "complex"}
BRINGUP_MODES = {"localization", "mapping", "navigation"}
INTERRUPT_REQUEST_TIMEOUT_SECONDS = 2.0
INTERRUPT_CONFIRM_TIMEOUT_SECONDS = 5.0
INACTIVE_NAVIGATION_STATES = TERMINAL_MISSION_STATES | {
    "idle",
    "inactive",
    "not_running",
    "stopped",
}


class OrsusError(RuntimeError):
    """Base class for controller errors."""


class ConfigError(OrsusError):
    """Raised when the YAML configuration is invalid."""


class ApiError(OrsusError):
    """Raised when an Orsus API returns an unsuccessful response."""


class TransportError(ApiError):
    """Raised when an HTTP exchange does not produce a response."""


@dataclass(frozen=True)
class HttpSettings:
    connect_timeout_seconds: float = 3.0
    read_timeout_seconds: float = 30.0
    long_operation_timeout_seconds: float = 300.0
    poll_interval_seconds: float = 1.0
    service_start_timeout_seconds: float = 60.0
    container_start_timeout_seconds: float = 180.0
    mission_timeout_seconds: float = 0.0
    read_retries: int = 2
    retry_backoff_seconds: float = 0.5

    @property
    def normal_timeout(self) -> tuple[float, float]:
        return (self.connect_timeout_seconds, self.read_timeout_seconds)

    @property
    def long_timeout(self) -> tuple[float, float]:
        return (self.connect_timeout_seconds, self.long_operation_timeout_seconds)


@dataclass(frozen=True)
class RobotConfig:
    name: str
    enabled: bool
    base_url: str
    expected_sn: str
    adapter_type: str
    scene_name: str
    bringup_mode: str
    relocalization_mode: str
    mission: Optional[dict[str, Any]]


@dataclass(frozen=True)
class AppConfig:
    http: HttpSettings
    robots: dict[str, RobotConfig]
    state_file: Path


def _number(value: Any, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ConfigError(f"{label} must be a number")
    return float(value)


def _positive_number(value: Any, label: str, *, allow_zero: bool = False) -> float:
    result = _number(value, label)
    if result < 0 or (result == 0 and not allow_zero):
        comparator = "non-negative" if allow_zero else "positive"
        raise ConfigError(f"{label} must be {comparator}")
    return result


def _integer(value: Any, label: str, *, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise ConfigError(f"{label} must be an integer >= {minimum}")
    return value


def _mapping(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, Mapping):
        raise ConfigError(f"{label} must be an object")
    return dict(value)


def _validate_pose(value: Any, label: str) -> dict[str, float]:
    pose = _mapping(value, label)
    missing = [key for key in ("x", "y", "theta") if key not in pose]
    if missing:
        raise ConfigError(f"{label} is missing: {', '.join(missing)}")
    return {
        "x": _number(pose["x"], f"{label}.x"),
        "y": _number(pose["y"], f"{label}.y"),
        "theta": _number(pose["theta"], f"{label}.theta"),
    }


def normalize_mission(value: Any, label: str = "mission") -> dict[str, Any]:
    mission = copy.deepcopy(_mapping(value, label))
    mode = mission.get("mode")
    if mode not in MISSION_MODES:
        raise ConfigError(f"{label}.mode must be one of {sorted(MISSION_MODES)}")

    mission["frame_id"] = mission.get("frame_id") or "map"
    if not isinstance(mission["frame_id"], str):
        raise ConfigError(f"{label}.frame_id must be a string")

    if mode in {"standard", "direct"}:
        mission["target"] = _validate_pose(mission.get("target"), f"{label}.target")
        for forbidden in ("waypoints", "steps", "cycles"):
            mission.pop(forbidden, None)
    elif mode == "route":
        waypoints = mission.get("waypoints")
        if not isinstance(waypoints, list) or not waypoints:
            raise ConfigError(f"{label}.waypoints must be a non-empty list")
        mission["waypoints"] = [
            _validate_pose(point, f"{label}.waypoints[{index}]")
            for index, point in enumerate(waypoints)
        ]
        mission["cycles"] = _integer(mission.get("cycles", 1), f"{label}.cycles", minimum=1)
        mission.pop("target", None)
        mission.pop("steps", None)
    else:
        steps = mission.get("steps")
        if not isinstance(steps, list) or not steps:
            raise ConfigError(f"{label}.steps must be a non-empty list")
        normalized_steps: list[dict[str, Any]] = []
        for index, raw_step in enumerate(steps):
            step_label = f"{label}.steps[{index}]"
            step = copy.deepcopy(_mapping(raw_step, step_label))
            step_type = step.get("type")
            if step_type not in {"navigate", "fixed_point", "rotate", "wait"}:
                raise ConfigError(
                    f"{step_label}.type must be navigate, fixed_point, rotate, or wait"
                )
            if step_type in {"navigate", "fixed_point"}:
                step["target"] = _validate_pose(step.get("target"), f"{step_label}.target")
            elif step_type == "rotate":
                step["theta"] = _number(step.get("theta"), f"{step_label}.theta")
            else:
                step["wait_seconds"] = _positive_number(
                    step.get("wait_seconds"), f"{step_label}.wait_seconds", allow_zero=True
                )

            on_failure = step.get("on_failure")
            if on_failure is not None and on_failure not in {"abort", "skip", "retry"}:
                raise ConfigError(f"{step_label}.on_failure must be abort, skip, or retry")
            if on_failure == "retry":
                step["retry"] = _integer(step.get("retry"), f"{step_label}.retry", minimum=1)
            normalized_steps.append(step)
        mission["steps"] = normalized_steps
        mission.pop("target", None)
        mission.pop("waypoints", None)
        mission.pop("cycles", None)
        if "default_params" in mission:
            mission["default_params"] = _mapping(
                mission["default_params"], f"{label}.default_params"
            )
    return mission


def _setting(raw: Mapping[str, Any], name: str, default: Any) -> Any:
    return raw[name] if name in raw else default


def load_config(path: Path) -> AppConfig:
    try:
        raw = yaml.safe_load(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ConfigError(f"config file not found: {path}") from exc
    except yaml.YAMLError as exc:
        raise ConfigError(f"invalid YAML in {path}: {exc}") from exc

    root = _mapping(raw, "config")
    raw_http = _mapping(root.get("http", {}), "http")
    settings = HttpSettings(
        connect_timeout_seconds=_positive_number(
            _setting(raw_http, "connect_timeout_seconds", 3.0), "http.connect_timeout_seconds"
        ),
        read_timeout_seconds=_positive_number(
            _setting(raw_http, "read_timeout_seconds", 30.0), "http.read_timeout_seconds"
        ),
        long_operation_timeout_seconds=_positive_number(
            _setting(raw_http, "long_operation_timeout_seconds", 300.0),
            "http.long_operation_timeout_seconds",
        ),
        poll_interval_seconds=_positive_number(
            _setting(raw_http, "poll_interval_seconds", 1.0), "http.poll_interval_seconds"
        ),
        service_start_timeout_seconds=_positive_number(
            _setting(raw_http, "service_start_timeout_seconds", 60.0),
            "http.service_start_timeout_seconds",
        ),
        container_start_timeout_seconds=_positive_number(
            _setting(raw_http, "container_start_timeout_seconds", 180.0),
            "http.container_start_timeout_seconds",
        ),
        mission_timeout_seconds=_positive_number(
            _setting(raw_http, "mission_timeout_seconds", 0.0),
            "http.mission_timeout_seconds",
            allow_zero=True,
        ),
        read_retries=_integer(_setting(raw_http, "read_retries", 2), "http.read_retries"),
        retry_backoff_seconds=_positive_number(
            _setting(raw_http, "retry_backoff_seconds", 0.5),
            "http.retry_backoff_seconds",
            allow_zero=True,
        ),
    )

    raw_robots = _mapping(root.get("robots"), "robots")
    if not raw_robots:
        raise ConfigError("robots must not be empty")
    robots: dict[str, RobotConfig] = {}
    for name, raw_robot_value in raw_robots.items():
        if not isinstance(name, str) or not name:
            raise ConfigError("robot names must be non-empty strings")
        raw_robot = _mapping(raw_robot_value, f"robots.{name}")
        base_url = str(raw_robot.get("base_url", "")).rstrip("/")
        parsed_url = urlparse(base_url)
        if parsed_url.scheme not in {"http", "https"} or not parsed_url.netloc:
            raise ConfigError(f"robots.{name}.base_url must be an HTTP(S) URL")
        expected_sn = str(raw_robot.get("expected_sn", "")).strip()
        adapter_type = str(raw_robot.get("adapter_type", "")).strip()
        if not expected_sn:
            raise ConfigError(f"robots.{name}.expected_sn is required")
        if not adapter_type:
            raise ConfigError(f"robots.{name}.adapter_type is required")
        bringup_mode = str(raw_robot.get("bringup_mode", "navigation")).strip()
        if bringup_mode not in BRINGUP_MODES:
            raise ConfigError(
                f"robots.{name}.bringup_mode must be one of {sorted(BRINGUP_MODES)}"
            )
        relocalization = _mapping(raw_robot.get("relocalization", {}), f"robots.{name}.relocalization")
        relocalization_mode = str(relocalization.get("mode", "sequential"))
        if relocalization_mode not in RELOCALIZATION_MODES:
            raise ConfigError(
                f"robots.{name}.relocalization.mode must be one of {sorted(RELOCALIZATION_MODES)}"
            )
        mission = raw_robot.get("mission")
        if mission is not None and not isinstance(mission, Mapping):
            raise ConfigError(f"robots.{name}.mission must be an object or null")
        robots[name] = RobotConfig(
            name=name,
            enabled=bool(raw_robot.get("enabled", True)),
            base_url=base_url,
            expected_sn=expected_sn,
            adapter_type=adapter_type,
            scene_name=str(raw_robot.get("scene_name", "")).strip(),
            bringup_mode=bringup_mode,
            relocalization_mode=relocalization_mode,
            mission=dict(mission) if mission is not None else None,
        )

    state_value = root.get("state_file", ".orsus_nav_state.json")
    if not isinstance(state_value, str) or not state_value.strip():
        raise ConfigError("state_file must be a non-empty string")
    state_file = Path(state_value)
    if not state_file.is_absolute():
        state_file = path.resolve().parent / state_file
    return AppConfig(http=settings, robots=robots, state_file=state_file)


def validate_robot_for_preflight(config: RobotConfig, *, require_mission: bool = False) -> None:
    if not config.scene_name or config.scene_name.upper() == "CHANGE_ME":
        raise ConfigError(f"robots.{config.name}.scene_name must select a real map")
    if config.mission is not None:
        normalize_mission(config.mission, f"robots.{config.name}.mission")
    elif require_mission:
        raise ConfigError(f"robots.{config.name}.mission is required for run")


class StateStore:
    def __init__(self, path: Path):
        self.path = path
        self._lock = threading.Lock()

    def _read_unlocked(self) -> dict[str, Any]:
        if not self.path.exists():
            return {"robots": {}}
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise ConfigError(f"cannot read state file {self.path}: {exc}") from exc
        if not isinstance(data, dict) or not isinstance(data.get("robots", {}), dict):
            raise ConfigError(f"invalid state file structure: {self.path}")
        data.setdefault("robots", {})
        return data

    def get(self, robot_name: str) -> dict[str, Any]:
        with self._lock:
            return dict(self._read_unlocked()["robots"].get(robot_name, {}))

    def update(self, robot_name: str, **values: Any) -> None:
        with self._lock:
            data = self._read_unlocked()
            robot = data["robots"].setdefault(robot_name, {})
            robot.update(values)
            robot["updated_at"] = datetime.now(timezone.utc).isoformat()
            self.path.parent.mkdir(parents=True, exist_ok=True)
            temp_path = self.path.with_name(f".{self.path.name}.{os.getpid()}.tmp")
            temp_path.write_text(
                json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
            )
            os.replace(temp_path, self.path)


class OrsusClient:
    API_PREFIX = "/v1/api"

    def __init__(
        self,
        config: RobotConfig,
        settings: HttpSettings,
        session: Optional[requests.Session] = None,
    ):
        self.config = config
        self.settings = settings
        self.session = session or requests.Session()
        self.session.headers.update({"Accept": "application/json"})

    def close(self) -> None:
        self.session.close()

    def _url(self, path: str, *, api: bool = True) -> str:
        prefix = self.API_PREFIX if api else ""
        return f"{self.config.base_url}{prefix}{path}"

    def _request_json(
        self,
        method: str,
        path: str,
        *,
        json_body: Optional[dict[str, Any]] = None,
        params: Optional[dict[str, Any]] = None,
        api: bool = True,
        retry_read: bool = False,
        long_operation: bool = False,
        envelope: bool = True,
    ) -> Any:
        url = self._url(path, api=api)
        attempts = self.settings.read_retries + 1 if retry_read else 1
        timeout = self.settings.long_timeout if long_operation else self.settings.normal_timeout
        last_error: Optional[BaseException] = None
        for attempt in range(attempts):
            try:
                response = self.session.request(
                    method,
                    url,
                    json=json_body,
                    params=params,
                    timeout=timeout,
                )
            except requests.RequestException as exc:
                last_error = exc
                if attempt + 1 < attempts:
                    self._backoff(attempt)
                    continue
                raise TransportError(
                    f"{self.config.name}: {method} {path} failed without a response: {exc}"
                ) from exc

            try:
                payload = response.json()
            except ValueError as exc:
                snippet = getattr(response, "text", "")[:300]
                raise ApiError(
                    f"{self.config.name}: {method} {path} returned non-JSON "
                    f"HTTP {response.status_code}: {snippet!r}"
                ) from exc

            if not 200 <= response.status_code < 300:
                if retry_read and response.status_code in {502, 503, 504} and attempt + 1 < attempts:
                    self._backoff(attempt)
                    continue
                code = payload.get("code") if isinstance(payload, dict) else None
                message = payload.get("msg") if isinstance(payload, dict) else payload
                raise ApiError(
                    f"{self.config.name}: {method} {path} returned HTTP {response.status_code}, "
                    f"code={code}, msg={message}"
                )

            if not envelope:
                return payload
            if not isinstance(payload, dict):
                raise ApiError(f"{self.config.name}: {method} {path} returned a non-object envelope")
            code = payload.get("code")
            if code not in (0, "0"):
                raise ApiError(
                    f"{self.config.name}: {method} {path} returned code={code}, "
                    f"msg={payload.get('msg')}"
                )
            data = payload.get("data")
            if isinstance(data, dict) and "code" in data and data["code"] not in (0, "0", None):
                raise ApiError(
                    f"{self.config.name}: {method} {path} downstream code={data['code']}, "
                    f"msg={data.get('msg')}"
                )
            return data

        raise TransportError(f"{self.config.name}: {method} {path} failed: {last_error}")

    def _backoff(self, attempt: int) -> None:
        delay = self.settings.retry_backoff_seconds * (2**attempt)
        if delay:
            time.sleep(delay)

    def health(self) -> dict[str, Any]:
        return self._request_json("GET", "/healthz", api=False, retry_read=True)

    def device(self) -> dict[str, Any]:
        return self._request_json("GET", "/systems/device", retry_read=True)

    def adapters(self) -> dict[str, Any]:
        return self._request_json("GET", "/services/motion/adapters", retry_read=True)

    def maps(self) -> dict[str, Any]:
        return self._request_json("GET", "/maps", retry_read=True)

    def landmarks(self, scene_name: str) -> list[dict[str, Any]]:
        data = self._request_json(
            "GET", "/nav/landmarks", params={"scene_name": scene_name}, retry_read=True
        )
        return data or []

    def swagger(self) -> dict[str, Any]:
        data = self._request_json(
            "GET",
            "/swagger/doc.json",
            api=False,
            retry_read=True,
            long_operation=True,
            envelope=False,
        )
        if not isinstance(data, dict):
            raise ApiError(f"{self.config.name}: Swagger document is not an object")
        return data

    def services_status(self) -> dict[str, Any]:
        return self._request_json("GET", "/services/status", retry_read=True)

    def motion_status(self) -> dict[str, Any]:
        return self._request_json("GET", "/services/motion/status", retry_read=True)

    def start_motion(self) -> Any:
        return self._request_json(
            "POST", "/services/motion/start", json_body={"adapter_type": self.config.adapter_type}
        )

    def stop_motion(self) -> Any:
        return self._request_json("POST", "/services/motion/stop")

    def scan_status(self) -> dict[str, Any]:
        return self._request_json("GET", "/services/scan/status", retry_read=True)

    def start_scan(self) -> Any:
        return self._request_json("POST", "/services/scan/start")

    def stop_scan(self) -> Any:
        return self._request_json("POST", "/services/scan/stop")

    def nav_container_status(self) -> dict[str, Any]:
        return self._request_json("GET", "/nav/container/status", retry_read=True)

    def start_nav_container(self) -> Any:
        return self._request_json(
            "POST",
            "/nav/container/start",
            json_body={
                "scene_name": self.config.scene_name,
                "bringup_mode": self.config.bringup_mode,
                "use_relocalization": True,
                "use_sim_time": False,
                "use_online_map": False,
            },
        )

    def stop_nav_container(self) -> Any:
        return self._request_json("POST", "/nav/container/stop")

    def current_map(self) -> Optional[str]:
        data = self._request_json(
            "POST",
            "/nav/get_nav_params",
            json_body={"names": ["general:map_name", "current_map"]},
            retry_read=True,
        )
        if not isinstance(data, dict):
            return None
        for name in ("general:map_name", "current_map"):
            result = data.get(name)
            if isinstance(result, dict) and result.get("success"):
                value = result.get("value")
                return str(value) if value is not None else None
        return None

    def enable_relocalization(self) -> Any:
        return self._request_json(
            "POST", "/nav/relocalization_toggle", json_body={"enable": True}
        )

    def global_relocalization(self) -> Any:
        return self._request_json(
            "POST",
            "/nav/global_relocalization",
            json_body={"mode": self.config.relocalization_mode},
            long_operation=True,
        )

    def cancel_global_relocalization(self) -> Any:
        return self._request_json("POST", "/nav/cancel_global_relocalization")

    def navigation_status(self) -> dict[str, Any]:
        return self._request_json("POST", "/nav/navigation_status", retry_read=True)

    def navigation_task_status(self) -> dict[str, Any]:
        return self._request_json("POST", "/nav/navigation_task_status", retry_read=True)

    def submit_mission(self, mission: dict[str, Any]) -> dict[str, Any]:
        return self._request_json("POST", "/nav/missions", json_body=mission)

    def mission_status(self, mission_id: str) -> dict[str, Any]:
        return self._request_json("GET", f"/nav/missions/{mission_id}", retry_read=True)

    def cancel_mission(self, mission_id: str) -> Any:
        return self._request_json("DELETE", f"/nav/missions/{mission_id}")

    def stop_navigation(self) -> Any:
        return self._request_json("POST", "/nav/stop_navigation")

    def pause_navigation(self) -> Any:
        return self._request_json("POST", "/nav/pause_navigation")

    def resume_navigation(self) -> Any:
        return self._request_json("POST", "/nav/resume_navigation")


def _device_sn(data: Mapping[str, Any]) -> str:
    value = data.get("sn", data.get("SN", ""))
    return str(value)


def _map_items(data: Any) -> list[dict[str, Any]]:
    if not isinstance(data, dict) or not isinstance(data.get("maps"), list):
        return []
    return [item for item in data["maps"] if isinstance(item, dict)]


def _motion_connected(data: Any, adapter_type: str) -> bool:
    if not isinstance(data, dict):
        return False
    detail = data.get("detail") if isinstance(data.get("detail"), dict) else {}
    state = str(detail.get("state", "")).upper()
    active = str(detail.get("active_adapter", "")).lower()
    return data.get("status") == "running" and state == "CONNECTED" and active == adapter_type.lower()


def _motion_ready(data: Any, adapter_type: str) -> tuple[bool, str]:
    if not _motion_connected(data, adapter_type):
        return False, "motion adapter is not CONNECTED"
    detail = data.get("detail", {})
    adapters = detail.get("adapters") if isinstance(detail, dict) else None
    if not isinstance(adapters, list):
        return True, "connected"
    for adapter in adapters:
        if not isinstance(adapter, dict):
            continue
        if str(adapter.get("robot_type", "")).lower() != adapter_type.lower():
            continue
        if adapter.get("available") is False:
            return False, f"active adapter is unavailable: {adapter.get('detail', '')}"
        health_detail = adapter.get("detail")
        if isinstance(health_detail, str):
            try:
                health_detail = json.loads(health_detail)
            except json.JSONDecodeError:
                health_detail = None
        if isinstance(health_detail, dict):
            if health_detail.get("transport_ready") is False:
                return False, f"CAN transport is not ready: {health_detail}"
            if health_detail.get("connected") is False:
                return False, f"adapter reports disconnected: {health_detail}"
        return True, "connected and healthy"
    return True, "connected; detailed adapter health is unavailable"


def _scan_ready(data: Any) -> bool:
    if not isinstance(data, dict) or data.get("status") in {"stopped", "unknown"}:
        return False
    detail = data.get("detail") if isinstance(data.get("detail"), dict) else {}
    services = detail.get("services") if isinstance(detail.get("services"), list) else []
    states = {
        str(item.get("name")): str(item.get("state", "")).upper()
        for item in services
        if isinstance(item, dict)
    }
    if not states:
        return data.get("status") == "running"
    return (
        states.get("gs-receiver") == "RUNNING"
        and states.get("sensors-tower") in {"RUNNING", "READY"}
        and states.get("websocket-server", "RUNNING") == "RUNNING"
    )


def _nav_running(data: Any) -> bool:
    return isinstance(data, dict) and data.get("running") is True


class DualRobotController:
    def __init__(
        self,
        config: AppConfig,
        selected: Iterable[RobotConfig],
        *,
        clients: Optional[Mapping[str, OrsusClient]] = None,
        progress_callback: Optional[Callable[[str, dict[str, Any]], None]] = None,
    ):
        self.config = config
        self.robots = {robot.name: robot for robot in selected}
        self.clients = dict(clients or {})
        for robot in self.robots.values():
            self.clients.setdefault(robot.name, OrsusClient(robot, config.http))
        self.state = StateStore(config.state_file)
        self.stop_event = threading.Event()
        self._active_lock = threading.Lock()
        self._active_missions: dict[str, str] = {}
        self._active_relocalizations: set[str] = set()
        self.progress_callback = progress_callback

    def close(self) -> None:
        for client in self.clients.values():
            client.close()

    def _emit_progress(self, robot_name: str, phase: str, status: str, **details: Any) -> None:
        if self.progress_callback is None:
            return
        event = {"phase": phase, "status": status, **details}
        try:
            self.progress_callback(robot_name, event)
        except Exception as exc:  # Progress reporting must never alter robot control.
            LOG.warning("%s: progress callback failed: %s", robot_name, exc)

    def _wait_for(
        self,
        robot_name: str,
        description: str,
        fetch: Callable[[], Any],
        predicate: Callable[[Any], bool],
        timeout_seconds: float,
    ) -> Any:
        deadline = time.monotonic() + timeout_seconds
        last_value: Any = None
        while time.monotonic() < deadline:
            if self.stop_event.is_set():
                raise InterruptedError(f"{robot_name}: interrupted while waiting for {description}")
            last_value = fetch()
            if predicate(last_value):
                return last_value
            time.sleep(self.config.http.poll_interval_seconds)
        raise ApiError(f"{robot_name}: timeout waiting for {description}; last={last_value}")

    def _parallel(self, operation: Callable[[RobotConfig, OrsusClient], Any]) -> dict[str, Any]:
        def invoke(robot: RobotConfig) -> tuple[str, dict[str, Any]]:
            try:
                data = operation(robot, self.clients[robot.name])
                successful = not (isinstance(data, dict) and data.get("success") is False)
                return robot.name, {"ok": successful, "data": data}
            except Exception as exc:  # Isolation is deliberate at the robot boundary.
                LOG.error("%s: %s", robot.name, exc)
                return robot.name, {
                    "ok": False,
                    "error": str(exc),
                    "error_type": type(exc).__name__,
                }

        executor = ThreadPoolExecutor(max_workers=max(1, len(self.robots)))
        futures = {executor.submit(invoke, robot): robot.name for robot in self.robots.values()}
        results: dict[str, Any] = {}
        try:
            for future in as_completed(futures):
                name, result = future.result()
                results[name] = result
        except KeyboardInterrupt:
            self.stop_event.set()
            for future in futures:
                future.cancel()
            executor.shutdown(wait=False, cancel_futures=True)
            raise
        else:
            executor.shutdown(wait=True)
        ordered = {name: results[name] for name in self.robots if name in results}
        return {"ok": all(item["ok"] for item in ordered.values()), "robots": ordered}

    def discover(self) -> dict[str, Any]:
        def one(robot: RobotConfig, client: OrsusClient) -> dict[str, Any]:
            device = client.device()
            adapters = client.adapters()
            map_data = client.maps()
            swagger = client.swagger()
            map_summaries = []
            for item in _map_items(map_data):
                name = str(item.get("name", ""))
                landmarks = client.landmarks(name) if name else []
                map_summaries.append(
                    {
                        "name": name,
                        "resolution": item.get("resolution"),
                        "origin": item.get("origin"),
                        "landmarks": [
                            {
                                "id": landmark.get("id"),
                                "name": landmark.get("name"),
                                "kind": landmark.get("kind"),
                                "point_count": len(landmark.get("points", []))
                                if isinstance(landmark.get("points"), list)
                                else 0,
                            }
                            for landmark in landmarks
                            if isinstance(landmark, dict)
                        ],
                    }
                )
            paths = swagger.get("paths") if isinstance(swagger.get("paths"), dict) else {}
            return {
                "device": device,
                "enabled_adapter_types": adapters.get("enabled_adapter_types", []),
                "global_relocalization_supported": "/nav/global_relocalization" in paths,
                "maps": map_summaries,
            }

        return self._parallel(one)

    def _preflight_one(
        self, robot: RobotConfig, client: OrsusClient, *, require_mission: bool
    ) -> dict[str, Any]:
        validate_robot_for_preflight(robot, require_mission=require_mission)
        health = client.health()
        if not isinstance(health, dict) or health.get("status") != "ok":
            raise ApiError(f"{robot.name}: unhealthy response: {health}")
        device = client.device()
        actual_sn = _device_sn(device)
        if actual_sn != robot.expected_sn:
            raise ConfigError(
                f"{robot.name}: expected SN {robot.expected_sn}, connected to {actual_sn or 'unknown'}"
            )
        adapters = client.adapters().get("enabled_adapter_types", [])
        if robot.adapter_type not in adapters:
            raise ConfigError(
                f"{robot.name}: adapter {robot.adapter_type!r} is unavailable; enabled={adapters}"
            )
        maps = [str(item.get("name")) for item in _map_items(client.maps())]
        if robot.scene_name not in maps:
            raise ConfigError(
                f"{robot.name}: map {robot.scene_name!r} is unavailable; maps={maps}"
            )
        swagger = client.swagger()
        paths = swagger.get("paths") if isinstance(swagger.get("paths"), dict) else {}
        if "/nav/global_relocalization" not in paths:
            raise ConfigError(
                f"{robot.name}: deployed API does not expose /nav/global_relocalization"
            )
        return {
            "success": True,
            "sn": actual_sn,
            "adapter_type": robot.adapter_type,
            "scene_name": robot.scene_name,
            "mission_configured": robot.mission is not None,
            "global_relocalization_supported": True,
        }

    def preflight(self, *, require_mission: bool = False) -> dict[str, Any]:
        return self._parallel(
            lambda robot, client: self._preflight_one(
                robot, client, require_mission=require_mission
            )
        )

    def _ensure_motion(self, robot: RobotConfig, client: OrsusClient) -> dict[str, Any]:
        current = client.motion_status()
        if _motion_ready(current, robot.adapter_type)[0]:
            return current
        detail = current.get("detail", {}) if isinstance(current, dict) else {}
        active = str(detail.get("active_adapter", "")).lower()
        if active not in {"", "unknown", robot.adapter_type.lower()}:
            raise ApiError(f"{robot.name}: another adapter is active: {active}")
        try:
            client.start_motion()
        except TransportError:
            current = client.motion_status()
            if not _motion_ready(current, robot.adapter_type)[0]:
                raise
            return current
        return self._wait_for(
            robot.name,
            "motion adapter connection",
            client.motion_status,
            lambda data: _motion_ready(data, robot.adapter_type)[0],
            self.config.http.service_start_timeout_seconds,
        )

    def _ensure_scan(self, robot: RobotConfig, client: OrsusClient) -> dict[str, Any]:
        current = client.scan_status()
        if _scan_ready(current):
            return current
        try:
            client.start_scan()
        except TransportError:
            current = client.scan_status()
            if not _scan_ready(current):
                raise
            return current
        return self._wait_for(
            robot.name,
            "scan services",
            client.scan_status,
            _scan_ready,
            self.config.http.service_start_timeout_seconds,
        )

    def _ensure_nav_container(self, robot: RobotConfig, client: OrsusClient) -> dict[str, Any]:
        current = client.nav_container_status()
        if not _nav_running(current):
            try:
                client.start_nav_container()
            except TransportError:
                current = client.nav_container_status()
                if not _nav_running(current):
                    raise
            current = self._wait_for(
                robot.name,
                "navigation container",
                client.nav_container_status,
                _nav_running,
                self.config.http.container_start_timeout_seconds,
            )

        self._wait_for_nav_api(robot, client)
        current_map = client.current_map()
        if current_map is None:
            raise ApiError(
                f"{robot.name}: navigation API is ready but its map cannot be verified"
            )
        if current_map != robot.scene_name:
            raise ApiError(
                f"{robot.name}: navigation container uses map {current_map!r}, "
                f"configured map is {robot.scene_name!r}; run shutdown first"
            )
        return current

    def _wait_for_nav_api(self, robot: RobotConfig, client: OrsusClient) -> dict[str, Any]:
        deadline = time.monotonic() + self.config.http.container_start_timeout_seconds
        last_error: Optional[ApiError] = None
        while time.monotonic() < deadline:
            if self.stop_event.is_set():
                raise InterruptedError(
                    f"{robot.name}: interrupted while waiting for navigation API readiness"
                )
            container = client.nav_container_status()
            if not _nav_running(container):
                raise ApiError(
                    f"{robot.name}: navigation container stopped before its API became ready; "
                    f"container={container}; last_error={last_error}"
                )
            try:
                return client.navigation_status()
            except ApiError as exc:
                last_error = exc
                time.sleep(self.config.http.poll_interval_seconds)
        raise ApiError(
            f"{robot.name}: timeout waiting for navigation API readiness; last_error={last_error}"
        )

    def _startup_one(self, robot: RobotConfig, client: OrsusClient) -> dict[str, Any]:
        self._emit_progress(robot.name, "preflight", "started")
        preflight = self._preflight_one(robot, client, require_mission=False)
        self._emit_progress(robot.name, "preflight", "completed")
        LOG.info("%s: starting motion adapter", robot.name)
        self._emit_progress(robot.name, "motion", "started")
        motion = self._ensure_motion(robot, client)
        self._emit_progress(robot.name, "motion", "completed")
        LOG.info("%s: starting scan services", robot.name)
        self._emit_progress(robot.name, "scan", "started")
        scan = self._ensure_scan(robot, client)
        self._emit_progress(robot.name, "scan", "completed")
        LOG.info("%s: starting navigation container", robot.name)
        self._emit_progress(robot.name, "navigation_container", "started")
        nav_container = self._ensure_nav_container(robot, client)
        self._emit_progress(robot.name, "navigation_container", "completed")
        client.enable_relocalization()
        LOG.info("%s: running %s global relocalization", robot.name, robot.relocalization_mode)
        self._emit_progress(
            robot.name,
            "relocalization",
            "started",
            mode=robot.relocalization_mode,
        )
        with self._active_lock:
            self._active_relocalizations.add(robot.name)
        try:
            try:
                relocalization = client.global_relocalization()
            except TransportError as exc:
                status = client.navigation_status()
                raise TransportError(
                    f"{robot.name}: global relocalization response is unknown; status={status}; {exc}"
                ) from exc
        finally:
            with self._active_lock:
                self._active_relocalizations.discard(robot.name)
        if self.stop_event.is_set():
            raise InterruptedError(f"{robot.name}: interrupted after global relocalization")
        self._emit_progress(robot.name, "relocalization", "completed")
        navigation = client.navigation_status()
        motion_after_relocalization = client.motion_status()
        motion_ready, motion_reason = _motion_ready(
            motion_after_relocalization, robot.adapter_type
        )
        if not motion_ready:
            raise ApiError(
                f"{robot.name}: motion adapter became unhealthy before mission submission: "
                f"{motion_reason}"
            )
        return {
            "success": True,
            "preflight": preflight,
            "motion": motion,
            "scan": scan,
            "nav_container": nav_container,
            "relocalization": relocalization,
            "navigation_status": navigation,
            "motion_after_relocalization": motion_after_relocalization,
        }

    def startup(self) -> dict[str, Any]:
        return self._parallel(self._startup_one)

    def _track_mission(self, robot: RobotConfig, client: OrsusClient, mission_id: str) -> dict[str, Any]:
        started = time.monotonic()
        last_status: Optional[str] = None
        while True:
            if self.stop_event.is_set():
                raise InterruptedError(f"{robot.name}: mission monitoring interrupted")
            status_data = client.mission_status(mission_id)
            if self.stop_event.is_set():
                raise InterruptedError(f"{robot.name}: mission monitoring interrupted")
            status = str(status_data.get("status", "unknown")).lower()
            self.state.update(robot.name, mission_id=mission_id, status=status)
            if status != last_status:
                self._emit_progress(
                    robot.name,
                    "mission",
                    status,
                    mission_id=mission_id,
                )
                last_status = status
            if status in TERMINAL_MISSION_STATES:
                with self._active_lock:
                    self._active_missions.pop(robot.name, None)
                return {
                    "success": status == "completed",
                    "mission_id": mission_id,
                    "status": status,
                    "mission": status_data,
                }
            motion = client.motion_status()
            if self.stop_event.is_set():
                raise InterruptedError(f"{robot.name}: mission monitoring interrupted")
            motion_ready, motion_reason = _motion_ready(motion, robot.adapter_type)
            if not motion_ready:
                cancel_error: Optional[str] = None
                try:
                    client.cancel_mission(mission_id)
                except ApiError as exc:
                    cancel_error = str(exc)
                with self._active_lock:
                    self._active_missions.pop(robot.name, None)
                self.state.update(
                    robot.name,
                    mission_id=mission_id,
                    status="motion_unhealthy",
                    motion_error=motion_reason,
                )
                return {
                    "success": False,
                    "mission_id": mission_id,
                    "status": "motion_unhealthy",
                    "mission": status_data,
                    "motion": motion,
                    "motion_error": motion_reason,
                    "cancel_error": cancel_error,
                }
            timeout = self.config.http.mission_timeout_seconds
            if timeout and time.monotonic() - started >= timeout:
                raise ApiError(f"{robot.name}: timeout waiting for mission {mission_id}; status={status}")
            time.sleep(self.config.http.poll_interval_seconds)

    def _run_one(self, robot: RobotConfig, client: OrsusClient) -> dict[str, Any]:
        validate_robot_for_preflight(robot, require_mission=True)
        startup = self._startup_one(robot, client)
        if self.stop_event.is_set():
            raise InterruptedError(f"{robot.name}: interrupted before mission submission")
        mission = normalize_mission(robot.mission, f"robots.{robot.name}.mission")
        LOG.info("%s: submitting %s mission", robot.name, mission["mode"])
        self._emit_progress(robot.name, "mission_submission", "started", mode=mission["mode"])
        try:
            submitted = client.submit_mission(mission)
        except TransportError as exc:
            aggregate_status: Any
            try:
                aggregate_status = client.navigation_task_status()
            except ApiError as status_exc:
                aggregate_status = {"query_error": str(status_exc)}
            self.state.update(
                robot.name,
                mission_id=None,
                status="submission_unknown",
                navigation_task_status=aggregate_status,
            )
            raise TransportError(
                f"{robot.name}: mission submission response is unknown and was not retried; "
                f"navigation_task_status={aggregate_status}; {exc}"
            ) from exc
        if not isinstance(submitted, dict) or not submitted.get("mission_id"):
            raise ApiError(f"{robot.name}: mission response has no mission_id: {submitted}")
        mission_id = str(submitted["mission_id"])
        with self._active_lock:
            self._active_missions[robot.name] = mission_id
        self.state.update(robot.name, mission_id=mission_id, status=submitted.get("status", "pending"))
        self._emit_progress(
            robot.name,
            "mission_submission",
            "completed",
            mission_id=mission_id,
        )
        result = self._track_mission(robot, client, mission_id)
        result["startup"] = startup
        return result

    def run(self) -> dict[str, Any]:
        return self._parallel(self._run_one)

    def resume_mission(self, mission_id: str) -> dict[str, Any]:
        if len(self.robots) != 1:
            raise ConfigError("resume_mission requires exactly one selected robot")

        def one(robot: RobotConfig, client: OrsusClient) -> dict[str, Any]:
            with self._active_lock:
                self._active_missions[robot.name] = mission_id
            self.state.update(robot.name, mission_id=mission_id, status="recovering")
            self._emit_progress(
                robot.name,
                "mission",
                "recovering",
                mission_id=mission_id,
            )
            return self._track_mission(robot, client, mission_id)

        return self._parallel(one)

    def status(self) -> dict[str, Any]:
        def one(robot: RobotConfig, client: OrsusClient) -> dict[str, Any]:
            result: dict[str, Any] = {
                "services": client.services_status(),
                "nav_container": client.nav_container_status(),
            }
            try:
                result["navigation"] = client.navigation_status()
            except ApiError as exc:
                result["navigation"] = {"available": False, "error": str(exc)}
            saved = self.state.get(robot.name)
            mission_id = saved.get("mission_id")
            if mission_id:
                try:
                    mission = client.mission_status(str(mission_id))
                    result["mission"] = mission
                    self.state.update(
                        robot.name,
                        mission_id=mission_id,
                        status=str(mission.get("status", "unknown")).lower(),
                    )
                except ApiError as exc:
                    result["mission_id"] = mission_id
                    result["mission_error"] = str(exc)
            return result

        return self._parallel(one)

    def pause(self) -> dict[str, Any]:
        return self._parallel(
            lambda robot, client: {"success": True, "response": client.pause_navigation()}
        )

    def resume(self) -> dict[str, Any]:
        return self._parallel(
            lambda robot, client: {"success": True, "response": client.resume_navigation()}
        )

    def cancel(self, mission_id: Optional[str] = None) -> dict[str, Any]:
        if mission_id is not None and len(self.robots) != 1:
            raise ConfigError("--mission-id can only be used when exactly one robot is selected")

        def one(robot: RobotConfig, client: OrsusClient) -> dict[str, Any]:
            selected_id = mission_id or self.state.get(robot.name).get("mission_id")
            if not selected_id:
                raise ConfigError(f"{robot.name}: no persisted mission_id; pass --mission-id")
            response = client.cancel_mission(str(selected_id))
            self.state.update(robot.name, mission_id=selected_id, status="cancelled")
            with self._active_lock:
                self._active_missions.pop(robot.name, None)
            return {"success": True, "mission_id": selected_id, "response": response}

        return self._parallel(one)

    def cancel_active_missions(self) -> dict[str, Any]:
        with self._active_lock:
            active = dict(self._active_missions)
        results: dict[str, Any] = {}
        for name, mission_id in active.items():
            robot = self.robots[name]
            emergency_client = OrsusClient(robot, self.config.http)
            try:
                emergency_client.cancel_mission(mission_id)
                self.state.update(name, mission_id=mission_id, status="cancelled")
                results[name] = {"ok": True, "mission_id": mission_id}
            except ApiError as exc:
                results[name] = {"ok": False, "mission_id": mission_id, "error": str(exc)}
            finally:
                emergency_client.close()
        return {"ok": all(item["ok"] for item in results.values()), "robots": results}

    def _interrupt_http_settings(self) -> HttpSettings:
        timeout = INTERRUPT_REQUEST_TIMEOUT_SECONDS
        return replace(
            self.config.http,
            connect_timeout_seconds=min(self.config.http.connect_timeout_seconds, timeout),
            read_timeout_seconds=min(self.config.http.read_timeout_seconds, timeout),
            long_operation_timeout_seconds=min(
                self.config.http.long_operation_timeout_seconds, timeout
            ),
            read_retries=0,
        )

    @staticmethod
    def _status_is_inactive(data: Any) -> bool:
        if not isinstance(data, dict):
            return False
        return str(data.get("status", "")).strip().lower() in INACTIVE_NAVIGATION_STATES

    def _confirm_interrupted_navigation(
        self,
        client: OrsusClient,
        mission_id: Optional[str],
    ) -> dict[str, Any]:
        deadline = time.monotonic() + INTERRUPT_CONFIRM_TIMEOUT_SECONDS
        interval = max(0.1, min(self.config.http.poll_interval_seconds, 0.5))
        last_mission: Any = None
        last_navigation: Any = None
        errors: list[str] = []
        while True:
            mission_inactive = mission_id is None
            try:
                last_navigation = client.navigation_task_status()
            except ApiError as exc:
                errors.append(f"navigation status: {exc}")

            if mission_id is not None:
                try:
                    last_mission = client.mission_status(mission_id)
                except ApiError as exc:
                    errors.append(f"mission status: {exc}")
                else:
                    mission_inactive = self._status_is_inactive(last_mission)

            if mission_inactive and self._status_is_inactive(last_navigation):
                return {
                    "confirmed": True,
                    "mission": last_mission,
                    "navigation": last_navigation,
                    "errors": errors,
                }
            if time.monotonic() >= deadline or errors:
                return {
                    "confirmed": False,
                    "mission": last_mission,
                    "navigation": last_navigation,
                    "errors": errors,
                }
            time.sleep(interval)

    def stop_and_cancel_active_missions(self) -> dict[str, Any]:
        self.stop_event.set()
        with self._active_lock:
            active = dict(self._active_missions)

        mission_ids: dict[str, Optional[str]] = {}
        for name in self.robots:
            mission_id = active.get(name)
            if mission_id is None:
                saved = self.state.get(name)
                saved_status = str(saved.get("status", "")).lower()
                if saved_status in ACTIVE_MISSION_STATES:
                    saved_id = saved.get("mission_id")
                    mission_id = str(saved_id) if saved_id else None
            mission_ids[name] = mission_id

        settings = self._interrupt_http_settings()

        def stop_one(robot: RobotConfig) -> tuple[str, dict[str, Any]]:
            mission_id = mission_ids[robot.name]
            client = OrsusClient(robot, settings)
            errors: list[str] = []
            stop_response: Any = None
            cancel_response: Any = None
            cancel_succeeded = False
            try:
                try:
                    stop_response = client.stop_navigation()
                except ApiError as exc:
                    errors.append(f"stop navigation: {exc}")

                if mission_id is not None:
                    try:
                        cancel_response = client.cancel_mission(mission_id)
                        cancel_succeeded = True
                    except ApiError as exc:
                        errors.append(f"cancel mission: {exc}")

                confirmation = self._confirm_interrupted_navigation(client, mission_id)
                if not confirmation["confirmed"]:
                    errors.append("navigation stop was not confirmed")

                if cancel_succeeded:
                    self.state.update(
                        robot.name,
                        mission_id=mission_id,
                        status="cancelled",
                        interrupt_cleanup_errors=errors,
                    )
                    with self._active_lock:
                        self._active_missions.pop(robot.name, None)
                elif mission_id is not None:
                    self.state.update(robot.name, interrupt_cleanup_errors=errors)

                return robot.name, {
                    "ok": not errors,
                    "mission_id": mission_id,
                    "stop_navigation": stop_response,
                    "cancel_mission": cancel_response,
                    "confirmation": confirmation,
                    "errors": errors,
                }
            finally:
                client.close()

        executor = ThreadPoolExecutor(max_workers=max(1, len(self.robots)))
        futures = {
            executor.submit(stop_one, robot): robot.name for robot in self.robots.values()
        }
        results: dict[str, dict[str, Any]] = {}
        try:
            for future in as_completed(futures):
                name = futures[future]
                try:
                    _, result = future.result()
                except Exception as exc:  # Keep the other robot's emergency stop independent.
                    result = {
                        "ok": False,
                        "mission_id": mission_ids[name],
                        "stop_navigation": None,
                        "cancel_mission": None,
                        "confirmation": None,
                        "errors": [f"interrupt cleanup: {exc}"],
                    }
                results[name] = result
        finally:
            executor.shutdown(wait=True)
        ordered = {name: results[name] for name in self.robots}
        return {"ok": all(item["ok"] for item in ordered.values()), "robots": ordered}

    def cancel_active_operations(self, *, stop_navigation: bool = False) -> dict[str, Any]:
        mission_results = (
            self.stop_and_cancel_active_missions()
            if stop_navigation
            else self.cancel_active_missions()
        )
        with self._active_lock:
            active_relocalizations = sorted(self._active_relocalizations)
        relocalization_results: dict[str, Any] = {}
        for name in active_relocalizations:
            emergency_client = OrsusClient(self.robots[name], self.config.http)
            try:
                response = emergency_client.cancel_global_relocalization()
                relocalization_results[name] = {"ok": True, "response": response}
            except ApiError as exc:
                relocalization_results[name] = {"ok": False, "error": str(exc)}
            finally:
                emergency_client.close()
        successful = mission_results["ok"] and all(
            item["ok"] for item in relocalization_results.values()
        )
        return {
            "ok": successful,
            "missions": mission_results["robots"],
            "relocalizations": relocalization_results,
        }

    def shutdown(self) -> dict[str, Any]:
        def one(robot: RobotConfig, client: OrsusClient) -> dict[str, Any]:
            errors: list[str] = []
            state = self.state.get(robot.name)
            mission_id = state.get("mission_id")
            try:
                if mission_id:
                    client.cancel_mission(str(mission_id))
                    self.state.update(robot.name, mission_id=mission_id, status="cancelled")
                else:
                    client.stop_navigation()
            except ApiError as exc:
                errors.append(f"stop navigation: {exc}")
            try:
                client.cancel_global_relocalization()
            except ApiError as exc:
                errors.append(f"cancel relocalization: {exc}")
            try:
                current = client.nav_container_status()
                if _nav_running(current):
                    client.stop_nav_container()
                    self._wait_for(
                        robot.name,
                        "navigation container shutdown",
                        client.nav_container_status,
                        lambda data: not _nav_running(data),
                        self.config.http.container_start_timeout_seconds,
                    )
            except ApiError as exc:
                errors.append(f"stop nav container: {exc}")
            try:
                client.stop_scan()
            except ApiError as exc:
                errors.append(f"stop scan: {exc}")
            try:
                client.stop_motion()
            except ApiError as exc:
                errors.append(f"stop motion: {exc}")
            return {"success": not errors, "errors": errors}

        return self._parallel(one)


def select_robots(config: AppConfig, names: list[str]) -> list[RobotConfig]:
    if names:
        unknown = sorted(set(names) - set(config.robots))
        if unknown:
            raise ConfigError(f"unknown robot(s): {', '.join(unknown)}")
        selected = [config.robots[name] for name in names]
    else:
        selected = [robot for robot in config.robots.values() if robot.enabled]
    if not selected:
        raise ConfigError("no robots selected")
    return selected


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path("robots.yaml"))
    parser.add_argument(
        "--robot",
        action="append",
        default=[],
        metavar="NAME",
        help="select one robot; repeat to select multiple (default: all enabled)",
    )
    parser.add_argument("--verbose", action="store_true")
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command, help_text in (
        ("discover", "read device identities, adapters, maps, and landmarks"),
        ("preflight", "perform all read-only readiness checks"),
        ("startup", "start services and perform global relocalization"),
        ("run", "startup, submit configured missions, and monitor them"),
        ("status", "query service, navigation, and persisted mission state"),
        ("pause", "pause current navigation"),
        ("resume", "resume current navigation"),
        ("shutdown", "cancel activity and stop navigation-related services"),
    ):
        subparsers.add_parser(command, help=help_text)
    cancel_parser = subparsers.add_parser("cancel", help="cancel a mission by persisted or explicit ID")
    cancel_parser.add_argument("--mission-id")
    return parser


def _print_report(report: Mapping[str, Any]) -> None:
    print(json.dumps(report, ensure_ascii=False, indent=2, default=str))


def main(argv: Optional[list[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )
    try:
        config = load_config(args.config)
        selected = select_robots(config, args.robot)
        controller = DualRobotController(config, selected)
    except ConfigError as exc:
        parser.error(str(exc))
        return 2

    try:
        if args.command == "discover":
            report = controller.discover()
        elif args.command == "preflight":
            report = controller.preflight(require_mission=False)
        elif args.command == "startup":
            report = controller.startup()
        elif args.command == "run":
            report = controller.run()
        elif args.command == "status":
            report = controller.status()
        elif args.command == "pause":
            report = controller.pause()
        elif args.command == "resume":
            report = controller.resume()
        elif args.command == "cancel":
            report = controller.cancel(args.mission_id)
        elif args.command == "shutdown":
            report = controller.shutdown()
        else:
            parser.error(f"unsupported command: {args.command}")
            return 2
        _print_report(report)
        return 0 if report.get("ok") else 1
    except KeyboardInterrupt:
        controller.stop_event.set()
        cancellation = controller.cancel_active_operations(stop_navigation=args.command == "run")
        print("\nInterrupted; active navigation cancellation result:", file=sys.stderr)
        _print_report(cancellation)
        return 130
    except ConfigError as exc:
        LOG.error("configuration error: %s", exc)
        return 2
    finally:
        controller.close()


if __name__ == "__main__":
    raise SystemExit(main())
