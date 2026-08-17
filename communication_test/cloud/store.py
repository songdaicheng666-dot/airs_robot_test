from __future__ import annotations

import json
import sqlite3
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


class StoreError(Exception):
    pass


class DuplicateConflict(StoreError):
    pass


class CommandNotFound(StoreError):
    pass


class InvalidStateTransition(StoreError):
    pass


class ActiveNavigationConflict(StoreError):
    pass


def utc_text(timestamp: float) -> str:
    return datetime.fromtimestamp(timestamp, tz=timezone.utc).isoformat().replace("+00:00", "Z")


class RelayStore:
    def __init__(self, database_path: Path, lease_seconds: int, command_ttl_seconds: int) -> None:
        self.database_path = database_path
        self.lease_seconds = lease_seconds
        self.command_ttl_seconds = command_ttl_seconds
        self._write_lock = threading.Lock()
        self.database_path.parent.mkdir(parents=True, exist_ok=True)
        self._initialize()

    def _connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.database_path, timeout=10, isolation_level=None)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys = ON")
        connection.execute("PRAGMA busy_timeout = 10000")
        return connection

    def _initialize(self) -> None:
        with self._connect() as connection:
            connection.execute("PRAGMA journal_mode = WAL")
            connection.executescript(
                """
                CREATE TABLE IF NOT EXISTS commands (
                    command_id TEXT PRIMARY KEY,
                    client_request_id TEXT NOT NULL UNIQUE,
                    device_id TEXT NOT NULL,
                    command_type TEXT NOT NULL,
                    payload_json TEXT NOT NULL,
                    state TEXT NOT NULL,
                    created_at REAL NOT NULL,
                    updated_at REAL NOT NULL,
                    expires_at REAL NOT NULL,
                    lease_until REAL,
                    delivery_count INTEGER NOT NULL DEFAULT 0,
                    result_json TEXT,
                    progress_json TEXT,
                    first_delivered_at REAL,
                    received_at REAL,
                    terminal_at REAL,
                    device_recorded_at TEXT,
                    error_message TEXT
                );
                CREATE INDEX IF NOT EXISTS idx_commands_delivery
                    ON commands(device_id, state, created_at);
                CREATE TABLE IF NOT EXISTS devices (
                    device_id TEXT PRIMARY KEY,
                    last_seen_at REAL NOT NULL,
                    telemetry_json TEXT NOT NULL
                );
                """
            )
            columns = {
                row["name"]
                for row in connection.execute("PRAGMA table_info(commands)").fetchall()
            }
            migrations = {
                "progress_json": "TEXT",
                "first_delivered_at": "REAL",
                "received_at": "REAL",
                "terminal_at": "REAL",
                "device_recorded_at": "TEXT",
            }
            for name, column_type in migrations.items():
                if name not in columns:
                    connection.execute(f"ALTER TABLE commands ADD COLUMN {name} {column_type}")

    @staticmethod
    def _optional_utc(row: sqlite3.Row, name: str) -> str | None:
        value = row[name]
        return utc_text(value) if value is not None else None

    @staticmethod
    def _command_dict(row: sqlite3.Row) -> dict[str, Any]:
        return {
            "command_id": row["command_id"],
            "client_request_id": row["client_request_id"],
            "device_id": row["device_id"],
            "type": row["command_type"],
            "payload": json.loads(row["payload_json"]),
            "state": row["state"],
            "created_at": utc_text(row["created_at"]),
            "updated_at": utc_text(row["updated_at"]),
            "expires_at": utc_text(row["expires_at"]),
            "first_delivered_at": RelayStore._optional_utc(row, "first_delivered_at"),
            "received_at": RelayStore._optional_utc(row, "received_at"),
            "terminal_at": RelayStore._optional_utc(row, "terminal_at"),
            "device_recorded_at": row["device_recorded_at"],
            "delivery_count": row["delivery_count"],
            "progress": json.loads(row["progress_json"]) if row["progress_json"] else None,
            "result": json.loads(row["result_json"]) if row["result_json"] else None,
            "error": row["error_message"],
        }

    def create_command(
        self,
        device_id: str,
        client_request_id: str,
        command_type: str,
        payload: dict[str, Any],
    ) -> tuple[dict[str, Any], bool]:
        payload_json = json.dumps(payload, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
        now = time.time()
        command_id = str(uuid.uuid4())
        with self._write_lock, self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            existing = connection.execute(
                "SELECT * FROM commands WHERE client_request_id = ?", (client_request_id,)
            ).fetchone()
            if existing is not None:
                if (
                    existing["device_id"] != device_id
                    or existing["command_type"] != command_type
                    or existing["payload_json"] != payload_json
                ):
                    connection.rollback()
                    raise DuplicateConflict("client_request_id was already used with different command data")
                connection.commit()
                return self._command_dict(existing), False

            if command_type == "CANCEL_NAVIGATION":
                target_id = payload.get("navigation_command_id")
                target = connection.execute(
                    "SELECT * FROM commands WHERE command_id = ? AND device_id = ?",
                    (target_id, device_id),
                ).fetchone()
                if target is None:
                    connection.rollback()
                    raise CommandNotFound(str(target_id))
                if target["command_type"] != "NAVIGATE":
                    connection.rollback()
                    raise InvalidStateTransition("cancel target is not a navigation command")

            if command_type == "NAVIGATE":
                connection.execute(
                    """
                    UPDATE commands
                    SET state = 'FAILED', updated_at = ?, terminal_at = COALESCE(terminal_at, ?),
                        error_message = 'command expired before receipt', lease_until = NULL
                    WHERE device_id = ? AND command_type = 'NAVIGATE'
                      AND state IN ('QUEUED', 'DELIVERED', 'RECEIVED', 'RUNNING')
                      AND expires_at <= ?
                    """,
                    (now, now, device_id, now),
                )
                active = connection.execute(
                    """
                    SELECT command_id FROM commands
                    WHERE device_id = ? AND command_type = 'NAVIGATE'
                      AND state IN ('QUEUED', 'DELIVERED', 'RECEIVED', 'RUNNING')
                    ORDER BY created_at ASC LIMIT 1
                    """,
                    (device_id,),
                ).fetchone()
                if active is not None:
                    connection.rollback()
                    raise ActiveNavigationConflict(
                        f"device already has active navigation command {active['command_id']}"
                    )

            connection.execute(
                """
                INSERT INTO commands (
                    command_id, client_request_id, device_id, command_type, payload_json,
                    state, created_at, updated_at, expires_at
                ) VALUES (?, ?, ?, ?, ?, 'QUEUED', ?, ?, ?)
                """,
                (
                    command_id,
                    client_request_id,
                    device_id,
                    command_type,
                    payload_json,
                    now,
                    now,
                    now + self.command_ttl_seconds,
                ),
            )
            row = connection.execute("SELECT * FROM commands WHERE command_id = ?", (command_id,)).fetchone()
            connection.commit()
        return self._command_dict(row), True

    def claim_next_command(self, device_id: str) -> dict[str, Any] | None:
        now = time.time()
        with self._write_lock, self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            connection.execute(
                """
                UPDATE commands
                SET state = 'FAILED', updated_at = ?, terminal_at = COALESCE(terminal_at, ?),
                    error_message = 'command expired before receipt', lease_until = NULL
                WHERE device_id = ? AND state IN ('QUEUED', 'DELIVERED', 'RECEIVED', 'RUNNING') AND expires_at <= ?
                """,
                (now, now, device_id, now),
            )
            row = connection.execute(
                """
                SELECT * FROM commands
                WHERE device_id = ?
                  AND (
                      state = 'QUEUED'
                      OR (state IN ('DELIVERED', 'RECEIVED', 'RUNNING') AND COALESCE(lease_until, 0) <= ?)
                  )
                  AND expires_at > ?
                ORDER BY created_at ASC
                LIMIT 1
                """,
                (device_id, now, now),
            ).fetchone()
            if row is None:
                connection.commit()
                return None
            connection.execute(
                """
                UPDATE commands
                SET state = 'DELIVERED', updated_at = ?, lease_until = ?,
                    first_delivered_at = COALESCE(first_delivered_at, ?),
                    delivery_count = delivery_count + 1
                WHERE command_id = ?
                """,
                (now, now + self.lease_seconds, now, row["command_id"]),
            )
            claimed = connection.execute(
                "SELECT * FROM commands WHERE command_id = ?", (row["command_id"],)
            ).fetchone()
            connection.commit()
        return self._command_dict(claimed)

    def update_command_state(
        self,
        device_id: str,
        command_id: str,
        new_state: str,
        result: dict[str, Any] | None,
        error_message: str | None,
        progress: dict[str, Any] | None = None,
        device_recorded_at: str | None = None,
    ) -> dict[str, Any]:
        now = time.time()
        with self._write_lock, self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute(
                "SELECT * FROM commands WHERE command_id = ? AND device_id = ?", (command_id, device_id)
            ).fetchone()
            if row is None:
                connection.rollback()
                raise CommandNotFound(command_id)

            current_state = row["state"]
            allowed = {
                "DELIVERED": {"RECEIVED", "RUNNING", "COMPLETED", "FAILED", "CANCELLED"},
                "RECEIVED": {"RECEIVED", "RUNNING", "COMPLETED", "FAILED", "CANCELLED"},
                "RUNNING": {"RUNNING", "COMPLETED", "FAILED", "CANCELLED"},
                "COMPLETED": {"COMPLETED"},
                "FAILED": {"FAILED"},
                "CANCELLED": {"CANCELLED"},
            }
            if new_state not in allowed.get(current_state, set()):
                connection.rollback()
                raise InvalidStateTransition(f"cannot change command from {current_state} to {new_state}")

            progress_json = (
                json.dumps(progress, ensure_ascii=False, separators=(",", ":"))
                if progress is not None
                else None
            )
            if new_state in {"RECEIVED", "RUNNING"}:
                connection.execute(
                    """
                    UPDATE commands SET
                        state = ?, updated_at = ?, lease_until = ?, expires_at = ?,
                        received_at = CASE WHEN ? = 'RECEIVED' THEN COALESCE(received_at, ?) ELSE received_at END,
                        progress_json = COALESCE(?, progress_json), result_json = NULL,
                        device_recorded_at = COALESCE(?, device_recorded_at), error_message = NULL
                    WHERE command_id = ?
                    """,
                    (
                        new_state,
                        now,
                        now + self.lease_seconds,
                        now + self.command_ttl_seconds,
                        new_state,
                        now,
                        progress_json,
                        device_recorded_at,
                        command_id,
                    ),
                )
            elif current_state != new_state:
                connection.execute(
                    """
                    UPDATE commands SET
                        state = ?, updated_at = ?, terminal_at = COALESCE(terminal_at, ?),
                        lease_until = NULL, progress_json = COALESCE(?, progress_json),
                        result_json = ?, device_recorded_at = COALESCE(?, device_recorded_at),
                        error_message = ?
                    WHERE command_id = ?
                    """,
                    (
                        new_state,
                        now,
                        now,
                        progress_json,
                        json.dumps(result, ensure_ascii=False, separators=(",", ":")) if result is not None else None,
                        device_recorded_at,
                        error_message,
                        command_id,
                    ),
                )
            updated = connection.execute("SELECT * FROM commands WHERE command_id = ?", (command_id,)).fetchone()
            connection.commit()
        return self._command_dict(updated)

    def validate_cancel_target(self, device_id: str, command_id: str) -> dict[str, Any]:
        command = self.get_command(command_id)
        if command["device_id"] != device_id:
            raise CommandNotFound(command_id)
        if command["type"] != "NAVIGATE":
            raise InvalidStateTransition("cancel target is not a navigation command")
        return command

    def get_command(self, command_id: str) -> dict[str, Any]:
        with self._connect() as connection:
            row = connection.execute("SELECT * FROM commands WHERE command_id = ?", (command_id,)).fetchone()
        if row is None:
            raise CommandNotFound(command_id)
        return self._command_dict(row)

    def record_telemetry(self, device_id: str, telemetry: dict[str, Any]) -> dict[str, Any]:
        now = time.time()
        telemetry_json = json.dumps(telemetry, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
        with self._write_lock, self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            connection.execute(
                """
                INSERT INTO devices (device_id, last_seen_at, telemetry_json)
                VALUES (?, ?, ?)
                ON CONFLICT(device_id) DO UPDATE SET
                    last_seen_at = excluded.last_seen_at,
                    telemetry_json = excluded.telemetry_json
                """,
                (device_id, now, telemetry_json),
            )
            connection.commit()
        return {"device_id": device_id, "received_at": utc_text(now)}

    def get_device_status(self, device_id: str, online_threshold_seconds: int) -> dict[str, Any]:
        now = time.time()
        with self._connect() as connection:
            row = connection.execute("SELECT * FROM devices WHERE device_id = ?", (device_id,)).fetchone()
        if row is None:
            return {
                "device_id": device_id,
                "online": False,
                "last_seen_at": None,
                "telemetry": None,
            }
        return {
            "device_id": device_id,
            "online": now - row["last_seen_at"] <= online_threshold_seconds,
            "last_seen_at": utc_text(row["last_seen_at"]),
            "telemetry": json.loads(row["telemetry_json"]),
        }
