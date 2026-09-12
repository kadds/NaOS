#!/usr/bin/env python3
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "util" / "make_root_image.py"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="naos-root-image-test-") as directory:
        directory = pathlib.Path(directory)
        source = directory / "root"
        source.joinpath("etc").mkdir(parents=True)
        source.joinpath("etc/init.sh").write_text("#!/bin/sh\n", encoding="utf-8")
        image = directory / "root.img"
        subprocess.run(
            [sys.executable, str(SCRIPT), "--input", str(source), "--output", str(image)],
            check=True,
        )

        boot = image.read_bytes()[:90]
        assert boot[13] == 16, "prepared root image must use 8 KiB clusters"
        assert boot[54:62] == b"FAT16   ", "prepared root image must use the compatible FAT16 layout"

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
