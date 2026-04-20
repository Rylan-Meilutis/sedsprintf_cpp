#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


HEADER_ENUMS_MARKER = "/* {{AUTOGEN:ENUMS}} */"
HEADER_ABI_MARKER = "/* {{AUTOGEN:ABI}} */"

RUST_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
ALL_CAPS_RE = re.compile(r"^[A-Z0-9_]+$")

DEFAULT_SRESULT = [
    ("SEDS_OK", 0),
    ("SEDS_ERR", -1),
    ("SEDS_GENERIC_ERROR", -2),
    ("SEDS_INVALID_TYPE", -3),
    ("SEDS_SIZE_MISMATCH", -4),
    ("SEDS_SIZE_MISMATCH_ERROR", -5),
    ("SEDS_EMPTY_ENDPOINTS", -6),
    ("SEDS_TIMESTAMP_INVALID", -7),
    ("SEDS_MISSING_PAYLOAD", -8),
    ("SEDS_HANDLER_ERROR", -9),
    ("SEDS_BAD_ARG", -10),
    ("SEDS_SERIALIZE", -11),
    ("SEDS_DESERIALIZE", -12),
    ("SEDS_IO", -13),
    ("SEDS_INVALID_UTF8", -14),
    ("SEDS_TYPE_MISMATCH", -15),
    ("SEDS_INVALID_LINK_ID", -16),
    ("SEDS_PACKET_TOO_LARGE", -17),
]

TIMESYNC_ENDPOINT = {
    "rust": "TimeSync",
    "name": "TIME_SYNC",
    "doc": "Time sync routing endpoint (always forwarded).",
    "link_local_only": False,
}

DISCOVERY_ENDPOINT = {
    "rust": "Discovery",
    "name": "DISCOVERY",
    "doc": "Discovery control endpoint for internal route advertisements.",
    "link_local_only": False,
}

ERROR_ENDPOINT = {
    "rust": "TelemetryError",
    "name": "TELEMETRY_ERROR",
    "doc": "Built-in TelemetryError endpoint",
    "link_local_only": False,
}

TIMESYNC_TYPES = [
    {
        "rust": "TimeSyncAnnounce",
        "name": "TIME_SYNC_ANNOUNCE",
        "doc": "Time source announce (priority, time_ms).",
        "class": "Data",
        "element": {"kind": "Static", "data_type": "UInt64", "count": 2},
        "endpoints": ["TimeSync"],
    },
    {
        "rust": "TimeSyncRequest",
        "name": "TIME_SYNC_REQUEST",
        "doc": "Time sync request (seq, t1_ms).",
        "class": "Data",
        "element": {"kind": "Static", "data_type": "UInt64", "count": 2},
        "endpoints": ["TimeSync"],
    },
    {
        "rust": "TimeSyncResponse",
        "name": "TIME_SYNC_RESPONSE",
        "doc": "Time sync response (seq, t1_ms, t2_ms, t3_ms).",
        "class": "Data",
        "element": {"kind": "Static", "data_type": "UInt64", "count": 4},
        "endpoints": ["TimeSync"],
    },
]

DISCOVERY_TYPES = [
    {
        "rust": "DiscoveryAnnounce",
        "name": "DISCOVERY_ANNOUNCE",
        "doc": "Endpoint discovery advertisement (dynamic list of endpoint IDs).",
        "class": "Data",
        "element": {"kind": "Dynamic", "data_type": "UInt32"},
        "endpoints": ["Discovery"],
    },
    {
        "rust": "DiscoveryTimeSyncSources",
        "name": "DISCOVERY_TIMESYNC_SOURCES",
        "doc": "Time sync source discovery advertisement (dynamic list of sender IDs).",
        "class": "Data",
        "element": {"kind": "Dynamic", "data_type": "UInt8"},
        "endpoints": ["Discovery"],
    },
    {
        "rust": "DiscoveryTopology",
        "name": "DISCOVERY_TOPOLOGY",
        "doc": "Full board-topology discovery advertisement (boards, endpoints, and connections).",
        "class": "Data",
        "element": {"kind": "Dynamic", "data_type": "UInt8"},
        "endpoints": ["Discovery"],
    },
]

RELIABLE_CONTROL_TYPES = [
    {
        "rust": "ReliableAck",
        "name": "RELIABLE_ACK",
        "doc": "Internal reliable-delivery acknowledgement (type, seq).",
        "class": "Data",
        "priority": 250,
        "element": {"kind": "Static", "data_type": "UInt32", "count": 2},
        "endpoints": ["TelemetryError"],
    },
    {
        "rust": "ReliablePartialAck",
        "name": "RELIABLE_PARTIAL_ACK",
        "doc": "Internal reliable-delivery selective acknowledgement (type, seq).",
        "class": "Data",
        "priority": 250,
        "element": {"kind": "Static", "data_type": "UInt32", "count": 2},
        "endpoints": ["TelemetryError"],
    },
    {
        "rust": "ReliablePacketRequest",
        "name": "RELIABLE_PACKET_REQUEST",
        "doc": "Internal reliable-delivery retransmit request (type, seq).",
        "class": "Data",
        "priority": 250,
        "element": {"kind": "Static", "data_type": "UInt32", "count": 2},
        "endpoints": ["TelemetryError"],
    },
]

ERROR_TYPE = {
    "rust": "TelemetryError",
    "name": "TELEMETRY_ERROR",
    "doc": "Built-in TelemetryError",
    "class": "Error",
    "element": {"kind": "Dynamic", "data_type": "String"},
    "endpoints": ["TelemetryError"],
}


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("--schema", required=True)
    p.add_argument("--header-template", required=True)
    p.add_argument("--header-out", required=True)
    p.add_argument("--schema-out", required=True)
    p.add_argument("--ipc-schema")
    p.add_argument("--abi-source")
    p.add_argument("--result-source")
    p.add_argument("--enable-timesync", choices=("0", "1"), default="1")
    p.add_argument("--enable-discovery", choices=("0", "1"), default="1")
    return p.parse_args()


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def normalize_endpoint(endpoint: dict) -> dict:
    ep = dict(endpoint)
    broadcast_mode = ep.pop("broadcast_mode", None)
    if broadcast_mode == "Never":
        ep["link_local_only"] = True
    elif "link_local_only" not in ep:
        ep["link_local_only"] = False
    return ep


def load_schema(path: Path) -> dict:
    cfg = load_json(path)
    cfg.setdefault("endpoints", [])
    cfg.setdefault("types", [])
    cfg["endpoints"] = [normalize_endpoint(ep) for ep in cfg["endpoints"]]
    cfg["types"] = [dict(ty) for ty in cfg["types"]]
    return cfg


def is_timesync_type(entry: dict) -> bool:
    return entry["rust"] in {"TimeSyncAnnounce", "TimeSyncRequest", "TimeSyncResponse"} or entry["name"] in {
        "TIME_SYNC_ANNOUNCE",
        "TIME_SYNC_REQUEST",
        "TIME_SYNC_RESPONSE",
    }


def is_discovery_type(entry: dict) -> bool:
    return entry["rust"] in {"DiscoveryAnnounce", "DiscoveryTimeSyncSources", "DiscoveryTopology"} or entry["name"] in {
        "DISCOVERY_ANNOUNCE",
        "DISCOVERY_TIMESYNC_SOURCES",
        "DISCOVERY_TOPOLOGY",
    }


def validate_schema(cfg: dict, *, timesync_enabled: bool, discovery_enabled: bool) -> None:
    endpoint_rust: set[str] = set()
    endpoint_name: set[str] = set()
    for ep in cfg["endpoints"]:
        rust = ep["rust"]
        name = ep["name"]
        if rust == "TelemetryError" or name == "TELEMETRY_ERROR":
            raise ValueError("TelemetryError endpoint is built-in and must not be defined in the schema")
        if timesync_enabled and (rust == "TimeSync" or name == "TIME_SYNC"):
            raise ValueError("TimeSync endpoint is built-in and must not be defined in the schema")
        if discovery_enabled and (rust == "Discovery" or name == "DISCOVERY"):
            raise ValueError("Discovery endpoint is built-in and must not be defined in the schema")
        if not RUST_IDENT_RE.match(rust):
            raise ValueError(f"invalid endpoint rust identifier: {rust!r}")
        if not ALL_CAPS_RE.match(name):
            raise ValueError(f"invalid endpoint schema name: {name!r}")
        if rust in endpoint_rust or name in endpoint_name:
            raise ValueError(f"duplicate endpoint definition: {rust}/{name}")
        endpoint_rust.add(rust)
        endpoint_name.add(name)

    valid_endpoints = set(endpoint_rust)
    valid_endpoints.add("TelemetryError")
    if timesync_enabled:
        valid_endpoints.add("TimeSync")
    if discovery_enabled:
        valid_endpoints.add("Discovery")

    type_rust: set[str] = set()
    type_name: set[str] = set()
    endpoint_link_local = {ep["rust"]: bool(ep.get("link_local_only")) for ep in cfg["endpoints"]}
    for ty in cfg["types"]:
        rust = ty["rust"]
        name = ty["name"]
        if rust == "TelemetryError" or name == "TELEMETRY_ERROR":
            raise ValueError("TelemetryError type is built-in and must not be defined in the schema")
        if timesync_enabled and is_timesync_type(ty):
            raise ValueError("TimeSync types are built-in and must not be defined in the schema")
        if discovery_enabled and is_discovery_type(ty):
            raise ValueError("Discovery types are built-in and must not be defined in the schema")
        if not RUST_IDENT_RE.match(rust):
            raise ValueError(f"invalid type rust identifier: {rust!r}")
        if not ALL_CAPS_RE.match(name):
            raise ValueError(f"invalid type schema name: {name!r}")
        if rust in type_rust or name in type_name:
            raise ValueError(f"duplicate type definition: {rust}/{name}")
        type_rust.add(rust)
        type_name.add(name)

        endpoints = ty.get("endpoints", [])
        if not endpoints:
            raise ValueError(f"type {rust} has no endpoints")
        saw_link_local = False
        saw_non_link_local = False
        for endpoint_name_rust in endpoints:
            if endpoint_name_rust not in valid_endpoints:
                raise ValueError(f"type {rust} references unknown endpoint {endpoint_name_rust!r}")
            if endpoint_link_local.get(endpoint_name_rust, False):
                saw_link_local = True
            else:
                saw_non_link_local = True
        if saw_link_local and saw_non_link_local:
            raise ValueError(f"type {rust} mixes link-local-only and normal endpoints")


def merge_ipc_overlay(base: dict, overlay: dict) -> dict:
    endpoint_rust = {ep["rust"] for ep in base["endpoints"]}
    endpoint_name = {ep["name"] for ep in base["endpoints"]}
    type_rust = {ty["rust"] for ty in base["types"]}
    type_name = {ty["name"] for ty in base["types"]}

    for ep in overlay["endpoints"]:
        ep = normalize_endpoint(ep)
        ep["link_local_only"] = True
        if ep["rust"] in endpoint_rust or ep["name"] in endpoint_name:
            raise ValueError(f"IPC overlay endpoint collides with base schema: {ep['rust']}/{ep['name']}")
        endpoint_rust.add(ep["rust"])
        endpoint_name.add(ep["name"])
        base["endpoints"].append(ep)

    for ty in overlay["types"]:
        if ty["rust"] in type_rust or ty["name"] in type_name:
            raise ValueError(f"IPC overlay type collides with base schema: {ty['rust']}/{ty['name']}")
        type_rust.add(ty["rust"])
        type_name.add(ty["name"])
        base["types"].append(dict(ty))
    return base


def finalize_schema(base: dict, *, ipc_schema: Path | None, timesync_enabled: bool, discovery_enabled: bool) -> dict:
    validate_schema(base, timesync_enabled=timesync_enabled, discovery_enabled=discovery_enabled)
    if ipc_schema is not None:
        base = merge_ipc_overlay(base, load_schema(ipc_schema))
    base["endpoints"].append(ERROR_ENDPOINT)
    base["types"].append(ERROR_TYPE)
    base["types"].extend(RELIABLE_CONTROL_TYPES)
    if timesync_enabled:
        base["endpoints"].append(TIMESYNC_ENDPOINT)
        base["types"].extend(TIMESYNC_TYPES)
    if discovery_enabled:
        base["endpoints"].append(DISCOVERY_ENDPOINT)
        base["types"].extend(DISCOVERY_TYPES)
    return base


def c_doc(text: str) -> str:
    return f"  /* {text.replace('*/', '* /')} */"


def render_c_enums(cfg: dict, result_members: list[tuple[str, int]]) -> str:
    out: list[str] = []
    out.append("typedef enum SedsDataType {")
    for i, ty in enumerate(cfg["types"]):
        if ty.get("doc"):
            out.append(c_doc(ty["doc"]))
        out.append(f"  SEDS_DT_{ty['name']} = {i},")
    out.append("} SedsDataType;\n")

    out.append("typedef enum SedsDataEndpoint {")
    for i, ep in enumerate(cfg["endpoints"]):
        if ep.get("doc"):
            out.append(c_doc(ep["doc"]))
        out.append(f"  SEDS_EP_{ep['name']} = {i},")
    out.append("} SedsDataEndpoint;\n")

    out.append("typedef enum SedsResult {")
    for name, value in result_members:
        out.append(f"  {name} = {value},")
    out.append("} SedsResult;")
    return "\n".join(out)


def parse_seds_result(path: Path | None) -> list[tuple[str, int]]:
    if path is None or not path.exists():
        return DEFAULT_SRESULT
    text = path.read_text(encoding="utf-8")
    match = re.search(r"typedef\s+enum\s+SedsResult\s*\{(.*?)\}\s*SedsResult\s*;", text, re.S)
    if not match:
        return DEFAULT_SRESULT
    members: list[tuple[str, int]] = []
    for name, value in re.findall(r"^\s*([A-Z0-9_]+)\s*=\s*(-?\d+)\s*,?\s*$", match.group(1), re.M):
        members.append((name, int(value)))
    return members or DEFAULT_SRESULT


def parse_abi_block(path: Path | None) -> str:
    if path is None or not path.exists():
        raise FileNotFoundError("missing ABI source")
    text = path.read_text(encoding="utf-8")
    match = re.search(
        r"/\* =================================================================\s+Public ABI wrappers \*AUTOGENERATED FROM RUST C ABI\*\s+================================================================= \*/(.*?)/\* ==============================\s+String / error formatting",
        text,
        re.S,
    )
    if not match:
        raise ValueError(f"could not extract ABI block from {path}")
    return match.group(1).strip()


def cpp_data_type(name: str) -> str:
    return {
        "NoData": "ElementDataType::NoData",
        "Bool": "ElementDataType::Bool",
        "UInt8": "ElementDataType::UInt8",
        "UInt16": "ElementDataType::UInt16",
        "UInt32": "ElementDataType::UInt32",
        "UInt64": "ElementDataType::UInt64",
        "UInt128": "ElementDataType::UInt128",
        "Int8": "ElementDataType::Int8",
        "Int16": "ElementDataType::Int16",
        "Int32": "ElementDataType::Int32",
        "Int64": "ElementDataType::Int64",
        "Int128": "ElementDataType::Int128",
        "Float32": "ElementDataType::Float32",
        "Float64": "ElementDataType::Float64",
        "String": "ElementDataType::String",
        "Binary": "ElementDataType::Binary",
    }[name]


def cpp_message_class(name: str) -> str:
    return {
        "Data": "MessageClass::Data",
        "Error": "MessageClass::Error",
        "Warning": "MessageClass::Warning",
    }[name]


def cpp_reliable_mode(value: str | None) -> str:
    return {
        None: "ReliableMode::None",
        "None": "ReliableMode::None",
        "Ordered": "ReliableMode::Ordered",
        "Unordered": "ReliableMode::Unordered",
    }[value]


def render_generated_schema(cfg: dict) -> str:
    data_type_size = {
        "UInt8": 1,
        "Int8": 1,
        "Bool": 1,
        "UInt16": 2,
        "Int16": 2,
        "UInt32": 4,
        "Int32": 4,
        "Float32": 4,
        "UInt64": 8,
        "Int64": 8,
        "Float64": 8,
        "UInt128": 16,
        "Int128": 16,
        "String": 1,
        "Binary": 1,
        "NoData": 0,
    }
    endpoint_index = {ep["rust"]: i for i, ep in enumerate(cfg["endpoints"])}
    lines: list[str] = []
    lines.append("#pragma once")
    lines.append('#include "src/internal.hpp"')
    lines.append("")
    lines.append("namespace seds::generated {")
    lines.append(f"inline constexpr uint32_t kEndpointCountValue = {len(cfg['endpoints'])}u;")
    lines.append("inline std::vector<const char*> make_endpoint_names() {")
    lines.append("    return {")
    for ep in cfg["endpoints"]:
        lines.append(f'        "{ep["name"]}",')
    lines.append("    };")
    lines.append("}")
    lines.append("inline std::vector<TypeInfo> make_type_info() {")
    lines.append("    return {")
    for ty in cfg["types"]:
        element = ty["element"]
        kind = element["kind"]
        dt = element["data_type"]
        count = element.get("count", 0)
        dynamic = "true" if kind == "Dynamic" else "false"
        reliable_mode = ty.get("reliable_mode")
        if ty.get("reliable") and reliable_mode is None:
            reliable_mode = "Ordered"
        link_local_only = "true" if all(bool(cfg["endpoints"][endpoint_index[name]].get("link_local_only")) for name in ty["endpoints"]) else "false"
        eps = ", ".join(str(endpoint_index[name]) for name in ty["endpoints"])
        lines.append(
            f'        TypeInfo{{"{ty["name"]}", {data_type_size[dt]}, {count}, {dynamic}, {cpp_reliable_mode(reliable_mode)}, '
            f'{cpp_data_type(dt)}, {cpp_message_class(ty["class"])}, {link_local_only}, {{{eps}}}}},'
        )
    lines.append("    };")
    lines.append("}")
    lines.append("}  // namespace seds::generated")
    return "\n".join(lines) + "\n"


def write_header(template_path: Path, out_path: Path, cfg: dict, *, abi_text: str, result_members: list[tuple[str, int]]) -> None:
    text = template_path.read_text(encoding="utf-8")
    text = text.replace(HEADER_ENUMS_MARKER, render_c_enums(cfg, result_members))
    text = text.replace(HEADER_ABI_MARKER, abi_text)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(text, encoding="utf-8")


def main() -> None:
    args = parse_args()
    timesync_enabled = args.enable_timesync == "1"
    discovery_enabled = args.enable_discovery == "1"
    schema = finalize_schema(
        load_schema(Path(args.schema)),
        ipc_schema=Path(args.ipc_schema) if args.ipc_schema else None,
        timesync_enabled=timesync_enabled,
        discovery_enabled=discovery_enabled,
    )
    abi_text = parse_abi_block(Path(args.abi_source) if args.abi_source else None)
    result_members = parse_seds_result(Path(args.result_source) if args.result_source else None)
    write_header(
        Path(args.header_template),
        Path(args.header_out),
        schema,
        abi_text=abi_text,
        result_members=result_members,
    )
    Path(args.schema_out).write_text(render_generated_schema(schema), encoding="utf-8")


if __name__ == "__main__":
    main()
