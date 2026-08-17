import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest.mock import MagicMock, patch

import requests

from orsus_nav import (
    ApiError,
    AppConfig,
    ConfigError,
    DualRobotController,
    HttpSettings,
    OrsusClient,
    RobotConfig,
    TransportError,
    main,
    normalize_mission,
)


def robot_config(
    name: str = "go2", *, mission: Any = None, bringup_mode: str = "navigation"
) -> RobotConfig:
    return RobotConfig(
        name=name,
        enabled=True,
        base_url=f"http://{name}.example:8898",
        expected_sn=f"SN-{name}",
        adapter_type=name,
        scene_name="test-map",
        bringup_mode=bringup_mode,
        relocalization_mode="sequential",
        mission=mission,
    )


class FakeResponse:
    def __init__(self, payload: Any, status_code: int = 200):
        self.payload = payload
        self.status_code = status_code
        self.text = str(payload)

    def json(self) -> Any:
        if isinstance(self.payload, ValueError):
            raise self.payload
        return self.payload


class FakeSession:
    def __init__(self, responses: list[Any]):
        self.responses = list(responses)
        self.calls: list[dict[str, Any]] = []
        self.headers: dict[str, str] = {}

    def request(self, method: str, url: str, **kwargs: Any) -> FakeResponse:
        self.calls.append({"method": method, "url": url, **kwargs})
        result = self.responses.pop(0)
        if isinstance(result, BaseException):
            raise result
        return result

    def close(self) -> None:
        return None


class MissionValidationTests(unittest.TestCase):
    def test_standard_defaults_to_map_frame(self) -> None:
        mission = normalize_mission(
            {"mode": "standard", "target": {"x": 1, "y": 2, "theta": 0}}
        )
        self.assertEqual(mission["frame_id"], "map")
        self.assertEqual(mission["target"], {"x": 1.0, "y": 2.0, "theta": 0.0})

    def test_route_requires_waypoints(self) -> None:
        with self.assertRaisesRegex(ConfigError, "non-empty"):
            normalize_mission({"mode": "route", "waypoints": []})

    def test_complex_validates_retry_count(self) -> None:
        with self.assertRaisesRegex(ConfigError, "retry"):
            normalize_mission(
                {
                    "mode": "complex",
                    "steps": [
                        {
                            "type": "rotate",
                            "theta": 1.0,
                            "on_failure": "retry",
                            "retry": 0,
                        }
                    ],
                }
            )

    def test_direct_rejects_missing_target(self) -> None:
        with self.assertRaisesRegex(ConfigError, "target"):
            normalize_mission({"mode": "direct"})


class HttpClientTests(unittest.TestCase):
    def setUp(self) -> None:
        self.settings = HttpSettings(read_retries=2, retry_backoff_seconds=0)
        self.robot = robot_config()

    def test_read_request_retries_transport_error(self) -> None:
        session = FakeSession(
            [
                requests.ConnectionError("temporary"),
                FakeResponse({"code": 0, "msg": "success", "data": {"status": "ok"}}),
            ]
        )
        client = OrsusClient(self.robot, self.settings, session=session)
        self.assertEqual(client.health(), {"status": "ok"})
        self.assertEqual(len(session.calls), 2)

    def test_write_request_is_not_retried(self) -> None:
        session = FakeSession([requests.Timeout("lost response")])
        client = OrsusClient(self.robot, self.settings, session=session)
        with self.assertRaises(TransportError):
            client.start_motion()
        self.assertEqual(len(session.calls), 1)

    def test_top_level_business_error_is_rejected(self) -> None:
        session = FakeSession(
            [FakeResponse({"code": 2001, "msg": "navigation failed", "data": None}, 500)]
        )
        client = OrsusClient(self.robot, self.settings, session=session)
        with self.assertRaisesRegex(ApiError, "code=2001"):
            client.nav_container_status()

    def test_nested_downstream_error_is_rejected(self) -> None:
        session = FakeSession(
            [
                FakeResponse(
                    {
                        "code": 0,
                        "msg": "success",
                        "data": {"code": 3, "msg": "adapter transaction in progress"},
                    }
                )
            ]
        )
        client = OrsusClient(self.robot, self.settings, session=session)
        with self.assertRaisesRegex(ApiError, "downstream code=3"):
            client.start_motion()

    def test_nav_container_uses_configured_bringup_mode(self) -> None:
        robot = robot_config("scout", bringup_mode="navigation")
        session = FakeSession([FakeResponse({"code": 0, "msg": "success", "data": {}})])
        client = OrsusClient(robot, self.settings, session=session)

        client.start_nav_container()

        self.assertEqual(session.calls[0]["json"]["bringup_mode"], "navigation")


class StubClient:
    def __init__(self, robot: RobotConfig):
        self.robot = robot
        self.sn = robot.expected_sn
        self.global_error: Exception | None = None
        self.submit_error: Exception | None = None
        self.submit_count = 0
        self.cancelled_missions: list[str] = []
        self.mission_states = ["completed"]
        self.motion_states: list[dict[str, Any]] = []
        self.navigation_status_errors: list[Exception] = []
        self.navigation_status_calls = 0
        self.interrupt_calls: list[str] = []
        self.stop_navigation_error: Exception | None = None
        self.cancel_error: Exception | None = None
        self.navigation_task_state = "running"
        self.stop_motion_count = 0

    def close(self) -> None:
        return None

    def health(self) -> dict[str, Any]:
        return {"status": "ok"}

    def device(self) -> dict[str, Any]:
        return {"sn": self.sn}

    def adapters(self) -> dict[str, Any]:
        return {"enabled_adapter_types": [self.robot.adapter_type]}

    def maps(self) -> dict[str, Any]:
        return {"maps": [{"name": self.robot.scene_name}]}

    def swagger(self) -> dict[str, Any]:
        return {"paths": {"/nav/global_relocalization": {"post": {}}}}

    def motion_status(self) -> dict[str, Any]:
        default = {
            "status": "running",
            "detail": {"state": "CONNECTED", "active_adapter": self.robot.adapter_type},
        }
        if not self.motion_states:
            return default
        return self.motion_states.pop(0) if len(self.motion_states) > 1 else self.motion_states[0]

    def scan_status(self) -> dict[str, Any]:
        return {
            "status": "running",
            "detail": {
                "services": [
                    {"name": "websocket-server", "state": "RUNNING"},
                    {"name": "gs-receiver", "state": "RUNNING"},
                    {"name": "sensors-tower", "state": "READY"},
                ]
            },
        }

    def nav_container_status(self) -> dict[str, Any]:
        return {"running": True, "status": "running"}

    def current_map(self) -> str:
        return self.robot.scene_name

    def enable_relocalization(self) -> None:
        return None

    def global_relocalization(self) -> dict[str, Any]:
        if self.global_error:
            raise self.global_error
        return {"mode": "sequential", "accum_time_msec": 100}

    def navigation_status(self) -> dict[str, Any]:
        self.navigation_status_calls += 1
        if self.navigation_status_errors:
            raise self.navigation_status_errors.pop(0)
        return {"status": "idle", "relocalization": "active"}

    def submit_mission(self, mission: dict[str, Any]) -> dict[str, Any]:
        self.submit_count += 1
        if self.submit_error:
            raise self.submit_error
        return {"mission_id": f"mission-{self.robot.name}", "status": "pending"}

    def navigation_task_status(self) -> dict[str, Any]:
        return {"status": self.navigation_task_state}

    def mission_status(self, mission_id: str) -> dict[str, Any]:
        state = self.mission_states.pop(0) if len(self.mission_states) > 1 else self.mission_states[0]
        return {"mission_id": mission_id, "status": state}

    def cancel_mission(self, mission_id: str) -> None:
        self.interrupt_calls.append("cancel_mission")
        if self.cancel_error:
            raise self.cancel_error
        self.cancelled_missions.append(mission_id)
        self.mission_states = ["cancelled"]

    def stop_navigation(self) -> dict[str, Any]:
        self.interrupt_calls.append("stop_navigation")
        if self.stop_navigation_error:
            raise self.stop_navigation_error
        self.navigation_task_state = "idle"
        return {"status": "stopped"}

    def stop_motion(self) -> None:
        self.stop_motion_count += 1


class ControllerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.settings = HttpSettings(poll_interval_seconds=0, retry_backoff_seconds=0)

    def app_config(self, robots: list[RobotConfig]) -> AppConfig:
        return AppConfig(
            http=self.settings,
            robots={robot.name: robot for robot in robots},
            state_file=Path(self.tempdir.name) / "state.json",
        )

    def test_preflight_failure_is_isolated_per_robot(self) -> None:
        go2 = robot_config("go2")
        scout = robot_config("scout")
        clients = {"go2": StubClient(go2), "scout": StubClient(scout)}
        clients["scout"].sn = "WRONG-SN"
        controller = DualRobotController(
            self.app_config([go2, scout]), [go2, scout], clients=clients
        )
        report = controller.preflight()
        self.assertFalse(report["ok"])
        self.assertTrue(report["robots"]["go2"]["ok"])
        self.assertFalse(report["robots"]["scout"]["ok"])

    def test_global_relocalization_failure_prevents_submission(self) -> None:
        mission = {"mode": "standard", "target": {"x": 1, "y": 2, "theta": 0}}
        go2 = robot_config("go2", mission=mission)
        client = StubClient(go2)
        client.global_error = ApiError("relocalization failed")
        controller = DualRobotController(
            self.app_config([go2]), [go2], clients={"go2": client}
        )
        report = controller.run()
        self.assertFalse(report["ok"])
        self.assertEqual(client.submit_count, 0)

    def test_startup_waits_for_navigation_api_readiness(self) -> None:
        scout = robot_config("scout")
        client = StubClient(scout)
        client.navigation_status_errors = [
            ApiError("navigation API connection refused"),
            ApiError("navigation API connection refused"),
        ]
        controller = DualRobotController(
            self.app_config([scout]), [scout], clients={"scout": client}
        )

        report = controller.startup()

        self.assertTrue(report["ok"])
        self.assertGreaterEqual(client.navigation_status_calls, 4)

    def test_one_robot_failure_does_not_stop_the_other(self) -> None:
        mission = {"mode": "standard", "target": {"x": 1, "y": 2, "theta": 0}}
        go2 = robot_config("go2", mission=mission)
        scout = robot_config("scout", mission=mission)
        go2_client = StubClient(go2)
        scout_client = StubClient(scout)
        scout_client.global_error = ApiError("relocalization failed")
        controller = DualRobotController(
            self.app_config([go2, scout]),
            [go2, scout],
            clients={"go2": go2_client, "scout": scout_client},
        )
        report = controller.run()
        self.assertFalse(report["ok"])
        self.assertTrue(report["robots"]["go2"]["ok"])
        self.assertFalse(report["robots"]["scout"]["ok"])
        self.assertEqual(go2_client.submit_count, 1)
        self.assertEqual(scout_client.submit_count, 0)

    def test_ambiguous_submission_is_not_retried(self) -> None:
        mission = {"mode": "standard", "target": {"x": 1, "y": 2, "theta": 0}}
        go2 = robot_config("go2", mission=mission)
        client = StubClient(go2)
        client.submit_error = TransportError("lost response")
        config = self.app_config([go2])
        controller = DualRobotController(config, [go2], clients={"go2": client})
        report = controller.run()
        self.assertFalse(report["ok"])
        self.assertEqual(client.submit_count, 1)
        persisted = json_from_path(config.state_file)
        self.assertEqual(persisted["robots"]["go2"]["status"], "submission_unknown")

    def test_mission_is_persisted_and_completes(self) -> None:
        mission = {
            "mode": "route",
            "cycles": 1,
            "waypoints": [
                {"x": 1, "y": 2, "theta": 0},
                {"x": 2, "y": 3, "theta": 1.5},
            ],
        }
        scout = robot_config("scout", mission=mission)
        client = StubClient(scout)
        client.mission_states = ["running", "completed"]
        config = self.app_config([scout])
        controller = DualRobotController(config, [scout], clients={"scout": client})
        report = controller.run()
        self.assertTrue(report["ok"])
        self.assertEqual(client.submit_count, 1)
        persisted = json_from_path(config.state_file)
        self.assertEqual(persisted["robots"]["scout"]["mission_id"], "mission-scout")
        self.assertEqual(persisted["robots"]["scout"]["status"], "completed")

    def test_progress_callback_reports_phases_and_resume_does_not_resubmit(self) -> None:
        mission = {"mode": "standard", "target": {"x": 1, "y": 2, "theta": 0}}
        go2 = robot_config("go2", mission=mission)
        client = StubClient(go2)
        client.mission_states = ["running", "completed"]
        events: list[tuple[str, dict[str, Any]]] = []
        controller = DualRobotController(
            self.app_config([go2]),
            [go2],
            clients={"go2": client},
            progress_callback=lambda name, event: events.append((name, event)),
        )

        report = controller.resume_mission("mission-existing")

        self.assertTrue(report["ok"])
        self.assertEqual(client.submit_count, 0)
        self.assertIn(
            ("go2", {"phase": "mission", "status": "recovering", "mission_id": "mission-existing"}),
            events,
        )
        self.assertEqual(events[-1][1]["status"], "completed")

    def test_failed_mission_marks_robot_failed(self) -> None:
        mission = {"mode": "direct", "target": {"x": 1, "y": 0, "theta": 0}}
        go2 = robot_config("go2", mission=mission)
        client = StubClient(go2)
        client.mission_states = ["failed"]
        controller = DualRobotController(
            self.app_config([go2]), [go2], clients={"go2": client}
        )
        report = controller.run()
        self.assertFalse(report["ok"])
        self.assertEqual(report["robots"]["go2"]["data"]["status"], "failed")

    def test_motion_failure_cancels_running_mission(self) -> None:
        mission = {"mode": "standard", "target": {"x": 1, "y": 0, "theta": 0}}
        scout = robot_config("scout", mission=mission)
        client = StubClient(scout)
        connected = {
            "status": "running",
            "detail": {"state": "CONNECTED", "active_adapter": "scout"},
        }
        disconnected = {
            "status": "stopped",
            "detail": {
                "state": "DISCONNECTED",
                "active_adapter": "scout",
                "adapters": [{"robot_type": "scout", "available": False}],
            },
        }
        client.motion_states = [connected, connected, disconnected]
        client.mission_states = ["running"]
        controller = DualRobotController(
            self.app_config([scout]), [scout], clients={"scout": client}
        )
        report = controller.run()
        self.assertFalse(report["ok"])
        result = report["robots"]["scout"]["data"]
        self.assertEqual(result["status"], "motion_unhealthy")
        self.assertEqual(client.cancelled_missions, ["mission-scout"])

    def test_cancel_uses_persisted_mission_id(self) -> None:
        go2 = robot_config("go2")
        client = StubClient(go2)
        config = self.app_config([go2])
        controller = DualRobotController(config, [go2], clients={"go2": client})
        controller.state.update("go2", mission_id="mission-123", status="running")
        report = controller.cancel()
        self.assertTrue(report["ok"])
        self.assertEqual(client.cancelled_missions, ["mission-123"])

    def test_interrupt_stops_navigation_before_cancelling_persisted_mission(self) -> None:
        go2 = robot_config("go2")
        client = StubClient(go2)
        config = self.app_config([go2])
        controller = DualRobotController(config, [go2], clients={"go2": client})
        controller.state.update("go2", mission_id="mission-123", status="running")

        with patch("orsus_nav.OrsusClient", return_value=client):
            report = controller.cancel_active_operations(stop_navigation=True)

        self.assertTrue(report["ok"])
        self.assertEqual(client.interrupt_calls, ["stop_navigation", "cancel_mission"])
        self.assertEqual(client.stop_motion_count, 0)
        persisted = json_from_path(config.state_file)
        self.assertEqual(persisted["robots"]["go2"]["status"], "cancelled")

    def test_interrupt_stops_navigation_without_a_mission_id(self) -> None:
        scout = robot_config("scout")
        client = StubClient(scout)
        controller = DualRobotController(
            self.app_config([scout]), [scout], clients={"scout": client}
        )

        with patch("orsus_nav.OrsusClient", return_value=client):
            report = controller.cancel_active_operations(stop_navigation=True)

        self.assertTrue(report["ok"])
        self.assertEqual(client.interrupt_calls, ["stop_navigation"])
        self.assertEqual(client.stop_motion_count, 0)

    def test_interrupt_cancel_continues_when_stop_navigation_fails(self) -> None:
        go2 = robot_config("go2")
        client = StubClient(go2)
        client.stop_navigation_error = ApiError("stop failed")
        config = self.app_config([go2])
        controller = DualRobotController(config, [go2], clients={"go2": client})
        controller.state.update("go2", mission_id="mission-123", status="running")

        with patch("orsus_nav.OrsusClient", return_value=client):
            report = controller.cancel_active_operations(stop_navigation=True)

        self.assertFalse(report["ok"])
        self.assertEqual(client.interrupt_calls, ["stop_navigation", "cancel_mission"])
        self.assertEqual(client.cancelled_missions, ["mission-123"])

    def test_interrupt_cleanup_is_independent_for_each_robot(self) -> None:
        go2 = robot_config("go2")
        scout = robot_config("scout")
        clients = {"go2": StubClient(go2), "scout": StubClient(scout)}
        config = self.app_config([go2, scout])
        controller = DualRobotController(config, [go2, scout], clients=clients)
        controller.state.update("go2", mission_id="mission-go2", status="running")
        controller.state.update("scout", mission_id="mission-scout", status="running")

        with patch(
            "orsus_nav.OrsusClient",
            side_effect=lambda robot, settings: clients[robot.name],
        ):
            report = controller.cancel_active_operations(stop_navigation=True)

        self.assertTrue(report["ok"])
        self.assertEqual(set(report["missions"]), {"go2", "scout"})
        for client in clients.values():
            self.assertEqual(client.interrupt_calls, ["stop_navigation", "cancel_mission"])
            self.assertEqual(client.stop_motion_count, 0)

    def test_main_run_interrupt_enables_navigation_stop_cleanup(self) -> None:
        go2 = robot_config("go2")
        config = self.app_config([go2])
        controller = MagicMock()
        controller.run.side_effect = KeyboardInterrupt
        controller.cancel_active_operations.return_value = {
            "ok": True,
            "missions": {},
            "relocalizations": {},
        }

        with (
            patch("orsus_nav.load_config", return_value=config),
            patch("orsus_nav.DualRobotController", return_value=controller),
        ):
            exit_code = main(["--config", "unused.yaml", "--robot", "go2", "run"])

        self.assertEqual(exit_code, 130)
        controller.stop_event.set.assert_called_once_with()
        controller.cancel_active_operations.assert_called_once_with(stop_navigation=True)
        controller.close.assert_called_once_with()


def json_from_path(path: Path) -> dict[str, Any]:
    import json

    return json.loads(path.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
