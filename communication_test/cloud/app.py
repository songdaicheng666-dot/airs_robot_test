from __future__ import annotations

import asyncio
import hmac
import math
import time
from datetime import datetime, timezone
from typing import Any, Literal
from uuid import UUID

from fastapi import Depends, FastAPI, Header, HTTPException, Request, Response, status
from pydantic import BaseModel, ConfigDict, Field, model_validator

from .config import DeviceSettings, Settings
from .store import (
    ActiveNavigationConflict,
    CommandNotFound,
    DuplicateConflict,
    InvalidStateTransition,
    RelayStore,
)


class StrictModel(BaseModel):
    model_config = ConfigDict(extra="forbid")


class CommandCreate(StrictModel):
    client_request_id: UUID
    type: Literal["PING", "STATUS_QUERY", "NAVIGATE", "CANCEL_NAVIGATION"]
    payload: dict[str, Any] = Field(default_factory=dict)

    @model_validator(mode="after")
    def validate_command_payload(self) -> "CommandCreate":
        if self.type == "NAVIGATE":
            parsed = NavigatePayload.model_validate(self.payload)
            self.payload = parsed.model_dump(mode="json")
        elif self.type == "CANCEL_NAVIGATION":
            parsed = CancelNavigationPayload.model_validate(self.payload)
            self.payload = parsed.model_dump(mode="json")
        return self


class CommandStateUpdate(StrictModel):
    state: Literal["RECEIVED", "RUNNING", "COMPLETED", "FAILED", "CANCELLED"]
    progress: dict[str, Any] | None = None
    result: dict[str, Any] | None = None
    error: str | None = Field(default=None, max_length=1024)
    device_recorded_at: datetime | None = None


class NavigationTarget(StrictModel):
    x: float
    y: float
    theta: float

    @model_validator(mode="after")
    def finite_values(self) -> "NavigationTarget":
        if not all(math.isfinite(value) for value in (self.x, self.y, self.theta)):
            raise ValueError("navigation coordinates must be finite")
        return self


class NavigatePayload(StrictModel):
    target: NavigationTarget


class CancelNavigationPayload(StrictModel):
    navigation_command_id: UUID


class FlightTelemetry(StrictModel):
    valid: bool
    status_code: int | None = None
    status: str | None = None
    display_mode_code: int | None = None


class PositionTelemetry(StrictModel):
    valid: bool
    latitude_deg: float | None = None
    longitude_deg: float | None = None
    altitude_ellipsoid_m: float | None = None
    visible_satellites: int | None = None


class GpsTelemetry(StrictModel):
    valid: bool
    fix_state: int | None = None
    horizontal_accuracy_m: float | None = None
    vertical_accuracy_m: float | None = None
    satellites_used: int | None = None


class RtkTelemetry(StrictModel):
    valid: bool
    connected: bool | None = None
    position_solution: int | None = None


class BatteryTelemetry(StrictModel):
    valid: bool
    percentage: int | None = Field(default=None, ge=0, le=100)
    voltage_v: float | None = None
    current_a: float | None = None


class M4tTelemetryCreate(StrictModel):
    recorded_at: datetime
    sequence: int = Field(ge=0)
    psdk_connected: bool
    flight: FlightTelemetry
    position: PositionTelemetry
    gps: GpsTelemetry
    rtk: RtkTelemetry
    battery: BatteryTelemetry
    errors: list[str] = Field(default_factory=list, max_length=16)


class OrsusAgentTelemetry(StrictModel):
    version: str = Field(min_length=1, max_length=32)


class OrsusDeviceTelemetry(StrictModel):
    model: str = Field(min_length=1, max_length=64)
    sn: str = Field(min_length=1, max_length=64)
    version: str = Field(min_length=1, max_length=64)


class OrsusEdgeCoreTelemetry(StrictModel):
    healthy: bool
    status: str | None = Field(default=None, max_length=64)


class OrsusRouteTelemetry(StrictModel):
    destination: str = Field(min_length=1, max_length=64)
    gateway: str | None = Field(default=None, max_length=64)
    interface: str = Field(min_length=1, max_length=32)
    source: str | None = Field(default=None, max_length=64)


class OrsusNetworkTelemetry(StrictModel):
    interface: str = Field(min_length=1, max_length=32)
    ipv4: str | None = Field(default=None, max_length=64)
    route_to_ecs: OrsusRouteTelemetry | None = None


class OrsusNavigationTelemetry(StrictModel):
    container: dict[str, Any] | None = None
    status: dict[str, Any] | None = None
    profile: dict[str, Any] | None = None


class OrsusTelemetryError(StrictModel):
    source: str = Field(min_length=1, max_length=64)
    message: str = Field(min_length=1, max_length=1024)


class OrsusTelemetryCreate(StrictModel):
    recorded_at: datetime
    sequence: int = Field(ge=0)
    platform: Literal["orsus"]
    agent: OrsusAgentTelemetry
    device: OrsusDeviceTelemetry
    edge_core: OrsusEdgeCoreTelemetry
    network: OrsusNetworkTelemetry
    services: dict[str, Any]
    navigation: OrsusNavigationTelemetry
    errors: list[OrsusTelemetryError] = Field(default_factory=list, max_length=16)


TelemetryCreate = M4tTelemetryCreate | OrsusTelemetryCreate


def create_app(settings: Settings | None = None) -> FastAPI:
    settings = settings or Settings.from_env()
    settings.validate()
    devices = settings.registered_devices
    store = RelayStore(settings.database_path, settings.command_lease_seconds, settings.command_ttl_seconds)
    command_available = asyncio.Condition()
    app = FastAPI(title="Edge Device Cloud Relay", version="3.0.0")
    app.state.settings = settings
    app.state.store = store

    def require_known_device(device_id: str) -> DeviceSettings:
        device = devices.get(device_id)
        if device is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="unknown device")
        return device

    def authenticate(expected_token: str):
        async def dependency(authorization: str | None = Header(default=None)) -> None:
            prefix = "Bearer "
            if authorization is None or not authorization.startswith(prefix):
                raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="missing bearer token")
            token = authorization[len(prefix):]
            if not hmac.compare_digest(token, expected_token):
                raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="invalid bearer token")

        return dependency

    async def authenticate_device(
        device_id: str,
        authorization: str | None = Header(default=None),
    ) -> DeviceSettings:
        device = require_known_device(device_id)
        prefix = "Bearer "
        if authorization is None or not authorization.startswith(prefix):
            raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="missing bearer token")
        token = authorization[len(prefix):]
        if not hmac.compare_digest(token, device.token):
            raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="invalid bearer token")
        return device

    operator_auth = authenticate(settings.operator_token)

    async def participant_auth(authorization: str | None = Header(default=None)) -> None:
        prefix = "Bearer "
        if authorization is None or not authorization.startswith(prefix):
            raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="missing bearer token")
        token = authorization[len(prefix):]
        candidates = [settings.operator_token, *(device.token for device in devices.values())]
        if not any(hmac.compare_digest(token, candidate) for candidate in candidates):
            raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="invalid bearer token")

    def require_safe_navigation_transport(request: Request) -> None:
        forwarded = request.headers.get("x-forwarded-proto", request.url.scheme)
        scheme = forwarded.split(",", 1)[0].strip().lower()
        if scheme != "https" and not settings.allow_insecure_navigation:
            raise HTTPException(
                status_code=status.HTTP_403_FORBIDDEN,
                detail="navigation over insecure HTTP is disabled",
            )

    @app.get("/health")
    async def health() -> dict[str, str]:
        return {"status": "ok"}

    @app.get("/v1/time")
    async def relay_time(_auth: None = Depends(participant_auth)) -> dict[str, Any]:
        now = datetime.now(tz=timezone.utc)
        return {"server_time": now.isoformat().replace("+00:00", "Z"), "unix_time_ns": time.time_ns()}

    @app.get("/v1/devices")
    async def list_devices(_auth: None = Depends(operator_auth)) -> dict[str, list[dict[str, Any]]]:
        result = []
        for device_id, device in sorted(devices.items()):
            item = store.get_device_status(device_id, settings.online_threshold_seconds)
            result.append(
                {
                    "device_id": device_id,
                    "device_type": device.kind,
                    "online": item["online"],
                    "last_seen_at": item["last_seen_at"],
                }
            )
        return {"devices": result}

    @app.post("/v1/devices/{device_id}/commands", status_code=status.HTTP_201_CREATED)
    async def submit_command(
        device_id: str,
        command_request: CommandCreate,
        response: Response,
        request: Request,
        _auth: None = Depends(operator_auth),
    ) -> dict[str, Any]:
        device = require_known_device(device_id)
        if command_request.type in {"NAVIGATE", "CANCEL_NAVIGATION"}:
            require_safe_navigation_transport(request)
            if device.kind != "orsus":
                raise HTTPException(
                    status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
                    detail="navigation commands require an Orsus device",
                )
        try:
            command, created = store.create_command(
                device_id,
                str(command_request.client_request_id),
                command_request.type,
                command_request.payload,
            )
        except CommandNotFound as exc:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="navigation command not found") from exc
        except (DuplicateConflict, ActiveNavigationConflict, InvalidStateTransition) as exc:
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail=str(exc)) from exc
        if not created:
            response.status_code = status.HTTP_200_OK
        async with command_available:
            command_available.notify_all()
        return command

    @app.get("/v1/devices/{device_id}/commands/next")
    async def next_command(
        device_id: str,
        response: Response,
        timeout_s: int = 25,
        _device: DeviceSettings = Depends(authenticate_device),
    ) -> dict[str, Any] | None:
        timeout_s = max(0, min(timeout_s, settings.max_poll_seconds))
        deadline = time.monotonic() + timeout_s
        while True:
            command = store.claim_next_command(device_id)
            if command is not None:
                return command
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                response.status_code = status.HTTP_204_NO_CONTENT
                return None
            try:
                async with command_available:
                    await asyncio.wait_for(command_available.wait(), timeout=remaining)
            except asyncio.TimeoutError:
                response.status_code = status.HTTP_204_NO_CONTENT
                return None

    @app.post("/v1/devices/{device_id}/commands/{command_id}/state")
    async def update_command_state(
        device_id: str,
        command_id: str,
        request: CommandStateUpdate,
        _device: DeviceSettings = Depends(authenticate_device),
    ) -> dict[str, Any]:
        try:
            return store.update_command_state(
                device_id,
                command_id,
                request.state,
                request.result,
                request.error,
                request.progress,
                request.device_recorded_at.isoformat().replace("+00:00", "Z")
                if request.device_recorded_at is not None
                else None,
            )
        except CommandNotFound as exc:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="command not found") from exc
        except InvalidStateTransition as exc:
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail=str(exc)) from exc

    @app.get("/v1/commands/{command_id}")
    async def get_command(command_id: str, _auth: None = Depends(operator_auth)) -> dict[str, Any]:
        try:
            return store.get_command(command_id)
        except CommandNotFound as exc:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="command not found") from exc

    @app.post("/v1/devices/{device_id}/telemetry", status_code=status.HTTP_202_ACCEPTED)
    async def post_telemetry(
        device_id: str,
        telemetry: TelemetryCreate,
        device: DeviceSettings = Depends(authenticate_device),
    ) -> dict[str, Any]:
        if device.kind == "m4t" and not isinstance(telemetry, M4tTelemetryCreate):
            raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="expected M4T telemetry")
        if device.kind == "orsus":
            if not isinstance(telemetry, OrsusTelemetryCreate):
                raise HTTPException(status_code=status.HTTP_422_UNPROCESSABLE_ENTITY, detail="expected Orsus telemetry")
            if not hmac.compare_digest(telemetry.device.sn, device.expected_sn or ""):
                raise HTTPException(
                    status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
                    detail="device SN does not match configured identity",
                )
        return store.record_telemetry(device_id, telemetry.model_dump(mode="json"))

    @app.get("/v1/devices/{device_id}/status")
    async def get_device_status(
        device_id: str,
        _auth: None = Depends(operator_auth),
    ) -> dict[str, Any]:
        device = require_known_device(device_id)
        result = store.get_device_status(device_id, settings.online_threshold_seconds)
        result["device_type"] = device.kind
        return result

    return app
