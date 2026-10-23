#!/usr/bin/env python3
"""Build and run language infrastructure tests only; no lab benchmarks are read."""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=root / "build")
    parser.add_argument("--config", default="Debug")
    parser.add_argument("--generator", help="optional CMake generator")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    cmake, ctest = shutil.which("cmake"), shutil.which("ctest")
    if not cmake or not ctest:
        parser.error("CMake and CTest must be available on PATH")
    build = args.build_dir.resolve()
    configure = [cmake, "-S", str(root), "-B", str(build),
                 "-DFDULANG_BUILD_LANGUAGE_TESTS=ON",
                 "-DFDULANG_BUILD_LAB_TESTS=OFF",
                 "-DPython3_EXECUTABLE=" + sys.executable,
                 "-DCMAKE_BUILD_TYPE=" + args.config]
    if args.generator:
        configure += ["-G", args.generator]
    commands = [(configure, root),
                ([cmake, "--build", str(build), "--config", args.config,
                  "--parallel", str(args.jobs)], root),
                ([ctest, "-C", args.config, "--output-on-failure", "-L", "^language$"], build)]
    try:
        for command, directory in commands:
            subprocess.run(command, check=True, cwd=directory)
    except (OSError, subprocess.CalledProcessError) as error:
        print("Language test run failed: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
