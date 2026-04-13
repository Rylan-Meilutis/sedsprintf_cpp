# Overview

`sedsprintf_cpp` mirrors the core telemetry runtime from `sedsprintf_rs` in C++.

Main responsibilities:
- schema-derived data type and endpoint metadata
- packet validation and string formatting
- compact wire serialization with CRC and reliable headers
- router / relay forwarding behavior
- built-in discovery and time sync control traffic

Public entrypoints:
- C ABI in `sedsprintf.h`
- C++ wrappers in `src/packet.hpp`, `src/router.hpp`, `src/relay.hpp`, `src/discovery.hpp`, and `src/timesync.hpp`

Non-goals in this repo:
- Rust FFI implementation
- Python bindings

Generated metadata comes from:
- `telemetry_config.json`
- optional IPC overlay schema files

The build regenerates:
- `build/generated/sedsprintf.h`
- `build/generated/generated_schema.hpp`

Overlay builds regenerate parallel files under `build/generated_overlay/`.
