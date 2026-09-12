#!/usr/bin/env python3
"""Generate disassembly files from debug artifacts in one NaOS build tree."""

from __future__ import annotations

import argparse
import os
import traceback

from build_paths import build_paths, resolve_build_directory
from mod import run_shell


def main() -> int:
    parser = argparse.ArgumentParser(description="debug tools: generate kernel debug files")
    parser.add_argument(
        "--build-dir",
        help="CMake build directory owning the debug artifacts (default: build or the only build-* directory)",
    )
    parser.add_argument("-d", "--detail", action="store_true", help="show source file and line information")
    parser.add_argument("-s", "--source", action="store_true", help="show source code")
    parser.add_argument("component", nargs="+", help="the components to decompile")
    args = parser.parse_args()

    try:
        paths = build_paths(resolve_build_directory(args.build_dir))
        debug_dir = paths.debug_dir
        file_map = {}
        for root, _, files in os.walk(debug_dir):
            for file in files:
                if os.path.splitext(file)[1] == ".dbg":
                    name = os.path.splitext(file)[0]
                    file_map[name] = os.path.join(root, file)

        missing = [component for component in args.component if component not in file_map]
        if missing:
            raise FileNotFoundError(
                f"debug component(s) not found in {debug_dir}: {', '.join(missing)}"
            )

        options = ""
        if args.detail:
            options += "-l "
        if args.source:
            options += "-S "
        command = f"objdump {options}-d --section=.text -C -l -m i386:x86-64"
        for component in args.component:
            source = file_map[component]
            output = os.path.join(os.path.dirname(source), f"{component}.dbg.S")
            run_shell(f'{command} "{source}" > "{output}"')
            print("find target at", source)
        print(f"{len(args.component)} target(s) generated.")
        return 0
    except Exception:
        traceback.print_exc()
        parser.print_help()
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
