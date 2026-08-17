from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


DEVICE_ID_PATTERN = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
DEVICE_KINDS = {"m4t", "orsus"}


@dataclass(frozen=True)
class DeviceSettings:
    device_id: str
    kind: str
    token: str
    expected_sn: str | None = None

    def validate(self) -> None:
        if not DEVICE_ID_PATTERN.fullmatch(self.device_id):
            raise ValueError(f"invalid device ID: {self.device_id!r}")
        if self.kind not in DEVICE_KINDS:
            raise ValueError(f"device {self.device_id!r} has unsupported kind {self.kind!r}")
        if len(self.token) < 32:
            raise ValueError(f"device {self.device_id!r} token must contain at least 32 characters")
        if self.kind == "orsus" and not self.expected_sn:
            raise ValueError(f"Orsus device {self.device_id!r} must define expected_sn")


def load_device_registry(path: Path) -> dict[str, DeviceSettings]:
    try:
        raw: Any = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"could not load device registry {path}: {exc}") from exc
    if not isinstance(raw, dict) or not raw:
        raise ValueError("device registry must be a non-empty JSON object")

    devices: dict[str, DeviceSettings] = {}
    for device_id, value in raw.items():
        if not isinstance(device_id, str) or not isinstance(value, dict):
            raise ValueError("device registry entries must map device IDs to objects")
        unknown = set(value) - {"kind", "token", "expected_sn"}
        if unknown:
            raise ValueError(f"device {device_id!r} contains unknown fields: {sorted(unknown)}")
        device = DeviceSettings(
            device_id=device_id,
            kind=value.get("kind", ""),
            token=value.get("token", ""),
            expected_sn=value.get("expected_sn"),
        )
        device.validate()
        devices[device_id] = device
    return devices


@dataclass(frozen=True)
class Settings:
    database_path: Path
    operator_token: str
    device_token: str = ""
    device_id: str = "M4T-001"
    online_threshold_seconds: int = 15
    command_lease_seconds: int = 35
    command_ttl_seconds: int = 300
    max_poll_seconds: int = 30
    devices: dict[str, DeviceSettings] = field(default_factory=dict)

    @classmethod
    def from_env(cls) -> "Settings":
        registry_path = os.environ.get("RELAY_DEVICE_CONFIG_PATH") or os.environ.get(
            "M4T_DEVICE_CONFIG_PATH"
        )
        devices = load_device_registry(Path(registry_path)) if registry_path else {}
        settings = cls(
            database_path=Path(os.environ.get("M4T_DATABASE_PATH", "communication_test/data/relay.db")),
            operator_token=os.environ.get("M4T_OPERATOR_TOKEN", ""),
            device_token=os.environ.get("M4T_DEVICE_TOKEN", ""),
            device_id=os.environ.get("M4T_DEVICE_ID", "M4T-001"),
            online_threshold_seconds=int(os.environ.get("M4T_ONLINE_THRESHOLD_SECONDS", "15")),
            command_lease_seconds=int(os.environ.get("M4T_COMMAND_LEASE_SECONDS", "35")),
            command_ttl_seconds=int(os.environ.get("M4T_COMMAND_TTL_SECONDS", "300")),
            max_poll_seconds=int(os.environ.get("M4T_MAX_POLL_SECONDS", "30")),
            devices=devices,
        )
        settings.validate()
        return settings

    @property
    def registered_devices(self) -> dict[str, DeviceSettings]:
        if self.devices:
            return self.devices
        return {
            self.device_id: DeviceSettings(
                device_id=self.device_id,
                kind="m4t",
                token=self.device_token,
            )
        }

    def validate(self) -> None:
        if len(self.operator_token) < 32:
            raise ValueError("operator token must contain at least 32 characters")
        devices = self.registered_devices
        if not devices:
            raise ValueError("at least one device must be configured")
        tokens = {self.operator_token}
        for key, device in devices.items():
            if key != device.device_id:
                raise ValueError("device registry key must match device_id")
            device.validate()
            if device.token in tokens:
                raise ValueError("operator and device tokens must all be different")
            tokens.add(device.token)
        if self.online_threshold_seconds <= 0:
            raise ValueError("online threshold must be positive")
        if self.max_poll_seconds <= 0:
            raise ValueError("maximum poll duration must be positive")
        if self.command_lease_seconds <= self.max_poll_seconds:
            raise ValueError("command lease must be longer than the maximum poll duration")
        if self.command_ttl_seconds <= self.command_lease_seconds:
            raise ValueError("command TTL must be longer than the command lease")
