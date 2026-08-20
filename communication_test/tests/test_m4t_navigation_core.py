from __future__ import annotations

import subprocess
from pathlib import Path


def test_m4t_navigation_core_host_build_and_rules(tmp_path: Path) -> None:
    root = Path(__file__).resolve().parents[2]
    source_dir = (
        root
        / "Payload-SDK-master"
        / "samples"
        / "sample_c"
        / "module_sample"
        / "m4t_cloud_relay"
    )
    executable = tmp_path / "m4t-navigation-core-test"
    subprocess.run(
        [
            "cc",
            "-std=c99",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{source_dir}",
            str(source_dir / "m4t_navigation_core.c"),
            str(Path(__file__).with_name("m4t_navigation_core_test.c")),
            "-lm",
            "-o",
            str(executable),
        ],
        check=True,
    )
    completed = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
    assert "tests passed" in completed.stdout
