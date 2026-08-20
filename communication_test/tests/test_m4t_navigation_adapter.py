from __future__ import annotations

import subprocess
from pathlib import Path


def test_m4t_navigation_with_injected_psdk_adapter(tmp_path: Path) -> None:
    root = Path(__file__).resolve().parents[2]
    psdk = root / "Payload-SDK-master"
    module_dir = psdk / "samples" / "sample_c" / "module_sample"
    relay_dir = module_dir / "m4t_cloud_relay"
    executable = tmp_path / "m4t-navigation-adapter-test"
    state_path = tmp_path / "navigation-state.json"
    subprocess.run(
        [
            "cc",
            "-std=gnu99",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{psdk / 'psdk_lib' / 'include'}",
            f"-I{module_dir}",
            f"-I{relay_dir}",
            str(relay_dir / "m4t_navigation.c"),
            str(relay_dir / "m4t_navigation_core.c"),
            str(module_dir / "utils" / "cJSON.c"),
            str(Path(__file__).with_name("m4t_navigation_adapter_test.c")),
            "-pthread",
            "-lm",
            "-o",
            str(executable),
        ],
        check=True,
    )
    completed = subprocess.run(
        [str(executable), str(state_path)],
        check=True,
        capture_output=True,
        text=True,
        timeout=20,
    )
    assert "adapter tests passed" in completed.stdout
