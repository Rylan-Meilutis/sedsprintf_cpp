#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path


USAGE = """Usage:
  build.py [configure] [test] [release] [clean] [target=<cmake-target>] [jobs=<n>]
           [build_dir=<dir>] [schema_path=<path>] [ipc_schema_path=<path>]
           [timesync=0|1] [discovery=0|1] [ctest_filter=<regex>]

Examples:
  build.py
  build.py test
  build.py release test
  build.py test ctest_filter=sedsprintf_rust_router_interop
  build.py configure build_dir=build/overlay ipc_schema_path=tests/schemas/ipc_link_local_overlay.json
  build.py target=sedsprintf_cpp_overlay_tests
"""

RUST_INTEROP_TEST = "sedsprintf_rust_router_interop"
RUST_INTEROP_TARGET = "sedsprintf_rust_interop_cpp_peer"


def run(cmd: list[str], cwd: Path) -> None:
    print("+", " ".join(cmd))
    subprocess.run(cmd, cwd=cwd, check=True)


def format_seconds(seconds: float) -> str:
    if seconds < 1:
        return f"{seconds * 1000:.0f}ms"
    if seconds < 60:
        return f"{seconds:.2f}s"
    minutes = int(seconds // 60)
    return f"{minutes}m{seconds - minutes * 60:.0f}s"


def print_banner(title: str) -> None:
    line = "-" * 60
    print(f"\n{line}\n{title}\n{line}")


def print_step(index: int, total: int, title: str) -> None:
    print_banner(f"{index}/{total} {title}")


def run_timed(cmd: list[str], cwd: Path) -> None:
    start = time.perf_counter()
    run(cmd, cwd)
    print(f"info: finished in {format_seconds(time.perf_counter() - start)}")


def configure_args(root: Path, build_dir: Path, options: dict[str, object], *, export_compile_commands: bool) -> list[str]:
    cmake_args = [
        "cmake",
        "-S",
        str(root),
        "-B",
        str(build_dir),
        f"-DCMAKE_BUILD_TYPE={'Release' if options['release'] else 'Debug'}",
    ]
    if export_compile_commands:
        cmake_args.append("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
    if options["schema_path"]:
        cmake_args.append(f"-DSEDSPRINTF_SCHEMA_PATH={(root / str(options['schema_path'])).resolve()}")
    if options["ipc_schema_path"]:
        cmake_args.append(f"-DSEDSPRINTF_IPC_SCHEMA_PATH={(root / str(options['ipc_schema_path'])).resolve()}")
    if options["timesync"] is not None:
        cmake_args.append(f"-DSEDSPRINTF_ENABLE_TIMESYNC={'ON' if options['timesync'] == '1' else 'OFF'}")
    if options["discovery"] is not None:
        cmake_args.append(f"-DSEDSPRINTF_ENABLE_DISCOVERY={'ON' if options['discovery'] == '1' else 'OFF'}")
    return cmake_args


def run_test_mode(root: Path, build_dir: Path, *, jobs: str | int | None, target: object,
                  ctest_filter: object) -> None:
    total_steps = 4
    print_banner("TEST MODE")

    print_step(1, total_steps, "codegen")
    codegen_cmd = ["cmake", "--build", str(build_dir), "--target", "sedsprintf_codegen", "sedsprintf_codegen_overlay"]
    if jobs:
        codegen_cmd.extend(["-j", str(jobs)])
    run_timed(codegen_cmd, root)

    print_step(2, total_steps, "cmake build")
    build_cmd = ["cmake", "--build", str(build_dir)]
    build_targets: list[str] = []
    if target:
        build_targets.append(str(target))
        selected_tests_can_include_interop = ctest_filter is None or re.search(str(ctest_filter), RUST_INTEROP_TEST)
        if selected_tests_can_include_interop and RUST_INTEROP_TARGET not in build_targets:
            build_targets.append(RUST_INTEROP_TARGET)
    if build_targets:
        build_cmd.extend(["--target", *build_targets])
    if jobs:
        build_cmd.extend(["-j", str(jobs)])
    run_timed(build_cmd, root)

    print_step(3, total_steps, "ctest")
    test_cmd = ["ctest", "--test-dir", str(build_dir), "--output-on-failure"]
    if ctest_filter:
        test_cmd.extend(["-R", str(ctest_filter)])
    run_timed(test_cmd, root)

    print_step(4, total_steps, "codegen check")
    run_timed([sys.executable, str(root / "tests" / "check_codegen.py")], root)


def help_and_exit(code: int = 0) -> int:
    stream = sys.stderr if code else sys.stdout
    print(USAGE, file=stream)
    return code


def parse_args(argv: list[str]) -> dict[str, object]:
    options: dict[str, object] = {
        "release": False,
        "test": False,
        "clean": False,
        "configure_only": False,
        "build_dir": None,
        "schema_path": None,
        "ipc_schema_path": None,
        "timesync": None,
        "discovery": None,
        "target": None,
        "jobs": None,
        "ctest_filter": None,
    }

    for arg in argv:
        if arg in {"-h", "--help", "help"}:
            raise SystemExit(help_and_exit())
        if arg == "release":
            options["release"] = True
        elif arg == "test":
            options["test"] = True
        elif arg == "clean":
            options["clean"] = True
        elif arg == "configure":
            options["configure_only"] = True
        elif arg.startswith("build_dir="):
            options["build_dir"] = arg.split("=", 1)[1]
        elif arg.startswith("schema_path="):
            options["schema_path"] = arg.split("=", 1)[1]
        elif arg.startswith("ipc_schema_path="):
            options["ipc_schema_path"] = arg.split("=", 1)[1]
        elif arg.startswith("timesync="):
            value = arg.split("=", 1)[1]
            if value not in {"0", "1"}:
                raise SystemExit(help_and_exit(1))
            options["timesync"] = value
        elif arg.startswith("discovery="):
            value = arg.split("=", 1)[1]
            if value not in {"0", "1"}:
                raise SystemExit(help_and_exit(1))
            options["discovery"] = value
        elif arg.startswith("target="):
            options["target"] = arg.split("=", 1)[1]
        elif arg.startswith("jobs="):
            options["jobs"] = arg.split("=", 1)[1]
        elif arg.startswith("ctest_filter="):
            options["ctest_filter"] = arg.split("=", 1)[1]
        else:
            print(f"error: unknown argument {arg!r}", file=sys.stderr)
            raise SystemExit(help_and_exit(1))
    return options


def remove_build_dir(build_dir: Path) -> None:
    if not build_dir.exists():
        return
    print(f"+ rm -rf {build_dir}")
    shutil.rmtree(build_dir)


def main(argv: list[str]) -> int:
    options = parse_args(argv)
    root = Path(__file__).resolve().parent
    build_dir = root / (options["build_dir"] or "build")
    build_dir.parent.mkdir(parents=True, exist_ok=True)

    if options["clean"]:
        remove_build_dir(build_dir)

    cmake_args = configure_args(root, build_dir, options, export_compile_commands=bool(options["test"]))

    start = time.perf_counter()
    run(cmake_args, root)
    print(f"info: configure finished in {format_seconds(time.perf_counter() - start)}")
    if options["configure_only"]:
        return 0

    jobs = options["jobs"] or os.cpu_count()
    if options["test"]:
        run_test_mode(root, build_dir, jobs=jobs, target=options["target"], ctest_filter=options["ctest_filter"])
        return 0

    build_cmd = ["cmake", "--build", str(build_dir)]
    if options["target"]:
        build_cmd.extend(["--target", str(options["target"])])
    if jobs:
        build_cmd.extend(["-j", str(jobs)])
    start = time.perf_counter()
    run(build_cmd, root)
    print(f"info: build finished in {format_seconds(time.perf_counter() - start)}")

    if options["test"]:
        test_cmd = ["ctest", "--test-dir", str(build_dir), "--output-on-failure"]
        if options["ctest_filter"]:
            test_cmd.extend(["-R", str(options["ctest_filter"])])
        start = time.perf_counter()
        run(test_cmd, root)
        print(f"info: tests finished in {format_seconds(time.perf_counter() - start)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
