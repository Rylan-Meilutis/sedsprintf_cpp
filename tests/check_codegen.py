#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path


def run(cmd: list[str], cwd: Path) -> None:
    subprocess.run(cmd, cwd=cwd, check=True)


def configure_build(root: Path, build_dir: Path, *, schema: Path | None = None, ipc_schema: Path | None = None,
                    timesync: bool | None = None, discovery: bool | None = None) -> None:
    cmd = [
        "cmake",
        "-S",
        str(root),
        "-B",
        str(build_dir),
        "-DCMAKE_BUILD_TYPE=Debug",
    ]
    if schema is not None:
        cmd.append(f"-DSEDSPRINTF_SCHEMA_PATH={schema}")
    if ipc_schema is not None:
        cmd.append(f"-DSEDSPRINTF_IPC_SCHEMA_PATH={ipc_schema}")
    if timesync is not None:
        cmd.append(f"-DSEDSPRINTF_ENABLE_TIMESYNC={'ON' if timesync else 'OFF'}")
    if discovery is not None:
        cmd.append(f"-DSEDSPRINTF_ENABLE_DISCOVERY={'ON' if discovery else 'OFF'}")
    run(cmd, root)


def build_target(root: Path, build_dir: Path, target: str) -> None:
    run(["cmake", "--build", str(build_dir), "--target", target], root)


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    fixtures = root / "tests" / "schemas"
    build_root = root / "build"
    build_root.mkdir(parents=True, exist_ok=True)

    tmp_build = Path(tempfile.mkdtemp(prefix="codegen_check_", dir=build_root))
    configure_build(root, tmp_build)
    build_target(root, tmp_build, "sedsprintf_codegen")
    header = tmp_build / "generated" / "sedsprintf.h"
    schema = tmp_build / "generated" / "generated_schema.hpp"
    assert header.exists(), header
    assert schema.exists(), schema
    htxt = header.read_text(encoding="utf-8")
    stxt = schema.read_text(encoding="utf-8")
    assert "SEDS_DT_GPS_DATA" in htxt
    assert "seds_router_log_f32_ex" in htxt
    assert "seds_router_log_typed" in htxt
    assert "seds_router_log_typed_ex" in htxt
    assert "make_type_info" in stxt
    assert "make_endpoint_names" in stxt

    override_schema = fixtures / "codegen_override_schema.json"
    override_build = Path(tempfile.mkdtemp(prefix="codegen_override_", dir=build_root))
    configure_build(root, override_build, schema=override_schema)
    build_target(root, override_build, "sedsprintf_codegen")
    override_header = (override_build / "generated" / "sedsprintf.h").read_text(encoding="utf-8")
    assert "SEDS_DT_CUSTOM_DATA" in override_header
    assert "SEDS_DT_GPS_DATA" not in override_header

    ipc_overlay = fixtures / "codegen_ipc_overlay.json"
    overlay_build = Path(tempfile.mkdtemp(prefix="codegen_overlay_", dir=build_root))
    configure_build(root, overlay_build, ipc_schema=ipc_overlay, timesync=False, discovery=False)
    build_target(root, overlay_build, "sedsprintf_codegen")
    overlay_header = (overlay_build / "generated" / "sedsprintf.h").read_text(encoding="utf-8")
    overlay_schema = (overlay_build / "generated" / "generated_schema.hpp").read_text(encoding="utf-8")
    assert "SEDS_EP_BOARD_LOCAL" in overlay_header
    assert "SEDS_DT_BOARD_LOCAL_FRAME" in overlay_header
    assert "SEDS_DT_TIME_SYNC_ANNOUNCE" not in overlay_header
    assert "SEDS_DT_DISCOVERY_ANNOUNCE" not in overlay_header
    assert "MessageClass::Warning" in overlay_schema
    assert "ElementDataType::Binary" in overlay_schema
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
