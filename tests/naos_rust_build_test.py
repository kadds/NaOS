#!/usr/bin/env python3
"""Regression test for the content-aware NaOS Rust build wrapper."""

from __future__ import annotations

import os
import stat
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "util"))

from naos_rust_build import main


def run_wrapper(workspace: Path, target: Path, fingerprint: Path, source: Path, cargo: Path) -> None:
    arguments = [
        "--workspace",
        str(workspace),
        "--target-dir",
        str(target),
        "--fingerprint-file",
        str(fingerprint),
        "--input-root",
        str(source.parent),
        "--",
        str(cargo),
        "build",
    ]
    assert main(arguments) == 0


def main_test() -> int:
    with tempfile.TemporaryDirectory(prefix="naos-rust-build-test-") as directory:
        root = Path(directory)
        workspace = root / "workspace"
        input_root = workspace / "rust"
        target = root / "target"
        fingerprint = target / ".naos-rust-input.sha256"
        log = root / "cargo.log"
        source = input_root / "library" / "std" / "build.rs"
        cargo = root / "fake-cargo"

        source.parent.mkdir(parents=True)
        target.mkdir()
        source.write_text("initial\n", encoding="utf-8")
        (target / "sentinel").write_text("keep\n", encoding="utf-8")
        cargo.write_text(
            "#!/usr/bin/env python3\n"
            "import os\n"
            "import pathlib\n"
            "import sys\n"
            "path = pathlib.Path(os.environ['NAOS_RUST_BUILD_TEST_LOG'])\n"
            "with path.open('a', encoding='utf-8') as stream:\n"
            "    stream.write(' '.join(sys.argv[1:]) + '\\n')\n",
            encoding="utf-8",
        )
        cargo.chmod(cargo.stat().st_mode | stat.S_IXUSR)
        os.environ["NAOS_RUST_BUILD_TEST_LOG"] = str(log)
        try:
            run_wrapper(workspace, target, fingerprint, source, cargo)
            assert log.read_text(encoding="utf-8").splitlines() == [
                "clean --target-dir " + str(target),
                "build",
            ]
            assert (target / "sentinel").exists()

            source.write_text("changed\n", encoding="utf-8")
            run_wrapper(workspace, target, fingerprint, source, cargo)
            assert log.read_text(encoding="utf-8").splitlines() == [
                "clean --target-dir " + str(target),
                "build",
                "clean --target-dir " + str(target),
                "build",
            ]
        finally:
            os.environ.pop("NAOS_RUST_BUILD_TEST_LOG", None)
    return 0


if __name__ == "__main__":
    raise SystemExit(main_test())
