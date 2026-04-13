# sedsprintf_cpp

C++ port of `sedsprintf_rs`, focused on the transport/runtime layers and the generated C ABI.

This repo provides:
- schema-driven telemetry packet types and endpoint metadata
- packet serialization / deserialization helpers
- router and relay implementations with discovery and time sync support
- C ABI plus RAII-style C++ wrappers
- GoogleTest coverage for packet, router, relay, overlay, and C interop behavior

The implementation is generated in part from `telemetry_config.json` and optional IPC overlay schemas.
Generated build artifacts live under `build/`.

## Features

- Discovery:
  learned reachable endpoints, adaptive announce cadence, selective forwarding, topology export
- Time sync:
  announce/request/response packets, discovered source routing, local network-time setters
- Reliable transport:
  ACK/retransmit support with schema-driven reliable mode metadata
- Link-local overlays:
  side-level isolation for software-bus / IPC-only traffic

Built-in control surfaces mirrored from the Rust runtime:
- data types:
  `TIME_SYNC_ANNOUNCE`, `TIME_SYNC_REQUEST`, `TIME_SYNC_RESPONSE`, `DISCOVERY_ANNOUNCE`,
  `DISCOVERY_TIMESYNC_SOURCES`, `TELEMETRY_ERROR`
- endpoints:
  `TIME_SYNC`, `DISCOVERY`, `TELEMETRY_ERROR`
- reliable modes:
  `None`, `Ordered`, `Unordered` from schema/codegen metadata

## Build

Primary entrypoint:

```sh
python3 build.py test
```

That runs:
- CMake configure with `compile_commands.json`
- `clang-tidy`
- project build
- `ctest`
- codegen verification

## Layout

- `src/`
  core runtime, wrappers, and generated-schema integration
- `tests/`
  GoogleTest unit/system tests and codegen checks
- `tests/schemas/`
  checked-in schema fixtures for overlay/codegen testing
- `scripts/generate_schema.py`
  schema/codegen driver
- `docs/`
  mirrored public-facing module documentation adapted from the Rust repo

## Status

Rust/Python interop layers are intentionally omitted here.
The remaining parity work is primarily broader Rust test coverage and any still-unported higher-level helper surfaces.
