#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
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
  build.py configure build_dir=build/overlay ipc_schema_path=tests/schemas/ipc_link_local_overlay.json
  build.py target=sedsprintf_cpp_overlay_tests
"""


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


def find_clang_tidy() -> str | None:
    candidates = [
        shutil.which("clang-tidy"),
        "/opt/homebrew/Cellar/llvm@20/20.1.8/bin/clang-tidy",
        "/opt/homebrew/Cellar/llvm/22.1.1/bin/clang-tidy",
        "/usr/local/opt/llvm/bin/clang-tidy",
    ]
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return candidate
    return None


def get_macos_sysroot(root: Path) -> str | None:
    try:
        completed = subprocess.run(
            ["xcrun", "--show-sdk-path"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    sysroot = completed.stdout.strip()
    return sysroot or None


def translation_units(root: Path) -> list[str]:
    units: list[str] = []
    for pattern in ("src/*.cpp", "tests/*.cpp", "tests/*.c"):
        units.extend(sorted(str(path) for path in root.glob(pattern)))
    return units


def filtered_compile_database(build_dir: Path, target: object) -> Path:
    compile_commands = json.loads((build_dir / "compile_commands.json").read_text(encoding="utf-8"))
    target_name = str(target) if target else ""
    prefer_overlay = "overlay" in target_name

    filtered: dict[str, dict[str, object]] = {}
    for entry in compile_commands:
        file_path = str(entry["file"])
        current = filtered.get(file_path)
        if current is None:
            filtered[file_path] = entry
            continue

        current_output = str(current.get("output", ""))
        next_output = str(entry.get("output", ""))
        current_is_overlay = "overlay" in current_output
        next_is_overlay = "overlay" in next_output
        if prefer_overlay:
            take_next = next_is_overlay and not current_is_overlay
        else:
            take_next = current_is_overlay and not next_is_overlay
        if take_next:
            filtered[file_path] = entry

    temp_dir = Path(tempfile.mkdtemp(prefix="clang_tidy_", dir=build_dir))
    temp_db = temp_dir / "compile_commands.json"
    temp_db.write_text(json.dumps(list(filtered.values()), indent=2), encoding="utf-8")
    return temp_dir


def run_clang_tidy(root: Path, build_dir: Path, *, target: object) -> None:
    clang_tidy = find_clang_tidy()
    if clang_tidy is None:
        raise RuntimeError("clang-tidy not found")

    tidy_db_dir = filtered_compile_database(build_dir, target)
    cmd = [clang_tidy, "-quiet", "-p", str(tidy_db_dir)]
    sysroot = get_macos_sysroot(root)
    if sysroot is not None:
        cmd.append(f"--extra-arg-before=-isysroot{sysroot}")
    cmd.extend(translation_units(root))
    run(cmd, root)


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
    total_steps = 5
    print_banner("TEST MODE")

    print_step(1, total_steps, "codegen")
    codegen_cmd = ["cmake", "--build", str(build_dir), "--target", "sedsprintf_codegen", "sedsprintf_codegen_overlay"]
    if jobs:
        codegen_cmd.extend(["-j", str(jobs)])
    run_timed(codegen_cmd, root)

    print_step(2, total_steps, "clang-tidy")
    start = time.perf_counter()
    run_clang_tidy(root, build_dir, target=target)
    print(f"info: finished in {format_seconds(time.perf_counter() - start)}")

    print_step(3, total_steps, "cmake build")
    build_cmd = ["cmake", "--build", str(build_dir)]
    if target:
        build_cmd.extend(["--target", str(target)])
    if jobs:
        build_cmd.extend(["-j", str(jobs)])
    run_timed(build_cmd, root)

    print_step(4, total_steps, "ctest")
    test_cmd = ["ctest", "--test-dir", str(build_dir), "--output-on-failure"]
    if ctest_filter:
        test_cmd.extend(["-R", str(ctest_filter)])
    run_timed(test_cmd, root)

    print_step(5, total_steps, "codegen check")
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
