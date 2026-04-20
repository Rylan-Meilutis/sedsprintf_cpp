#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import textwrap
from pathlib import Path


DEFAULT_RUST_REPO_URL = "https://github.com/Rylan-Meilutis/sedsprintf_rs.git"


RUST_MAIN = r'''
use sedsprintf_rs::config::{DataEndpoint, DataType};
use sedsprintf_rs::relay::{Relay, RelaySideOptions};
use sedsprintf_rs::router::{Clock, EndpointHandler, Router, RouterConfig, RouterSideOptions};
use sedsprintf_rs::timesync::{TimeSyncConfig, TimeSyncRole};
use sedsprintf_rs::TelemetryResult;
use std::io::{self, BufRead};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

#[derive(Clone)]
struct SharedClock {
    now: Arc<AtomicU64>,
}

impl Clock for SharedClock {
    fn now_ms(&self) -> u64 {
        self.now.load(Ordering::Relaxed)
    }
}

fn hex_encode(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn hex_decode(hex: &str) -> TelemetryResult<Vec<u8>> {
    if hex.len() % 2 != 0 {
        return Err(sedsprintf_rs::TelemetryError::Deserialize("odd hex length"));
    }
    let mut out = Vec::with_capacity(hex.len() / 2);
    for idx in (0..hex.len()).step_by(2) {
        let byte = u8::from_str_radix(&hex[idx..idx + 2], 16)
            .map_err(|_| sedsprintf_rs::TelemetryError::Deserialize("bad hex"))?;
        out.push(byte);
    }
    Ok(out)
}

fn capture_side() -> (Arc<Mutex<Vec<Vec<u8>>>>, impl Fn(&[u8]) -> TelemetryResult<()> + Send + Sync + 'static) {
    let captured = Arc::new(Mutex::new(Vec::<Vec<u8>>::new()));
    let out = captured.clone();
    (captured, move |bytes| {
        out.lock().unwrap().push(bytes.to_vec());
        Ok(())
    })
}

fn emit() -> TelemetryResult<()> {
    let (captured, tx) = capture_side();
    let router = Router::new_with_clock(
        RouterConfig::default().with_sender("RUST_INTEROP"),
        Box::new(|| 123_u64),
    );
    router.add_side_serialized("cpp", tx);
    router.log_ts(DataType::GpsData, 123, &[11.25_f32, -2.5, 99.0])?;
    let frames = captured.lock().unwrap();
    let bytes = frames.last().ok_or(sedsprintf_rs::TelemetryError::Io("rust emitted no frame"))?;
    println!("{}", hex_encode(bytes));
    Ok(())
}

fn consume(hex: &str) -> TelemetryResult<()> {
    let values = Arc::new(Mutex::new(None::<Vec<f32>>));
    let seen = values.clone();
    let handler = EndpointHandler::new_packet_handler(DataEndpoint::Radio, move |pkt| {
        *seen.lock().unwrap() = Some(pkt.data_as_f32()?);
        Ok(())
    });
    let router = Router::new_with_clock(
        RouterConfig::new([handler]).with_sender("RUST_INTEROP"),
        Box::new(|| 123_u64),
    );
    let bytes = hex_decode(hex)?;
    router.rx_serialized(&bytes)?;
    let got = values
        .lock()
        .unwrap()
        .clone()
        .ok_or(sedsprintf_rs::TelemetryError::Io("rust received no packet"))?;
    println!("{} {} {}", got[0], got[1], got[2]);
    Ok(())
}

fn consume_reliable(hex: &str) -> TelemetryResult<()> {
    let (captured, tx) = capture_side();
    let values = Arc::new(Mutex::new(None::<Vec<f32>>));
    let seen = values.clone();
    let handler = EndpointHandler::new_packet_handler(DataEndpoint::Radio, move |pkt| {
        *seen.lock().unwrap() = Some(pkt.data_as_f32()?);
        Ok(())
    });
    let router = Router::new_with_clock(
        RouterConfig::new([handler]).with_sender("RUST_RELIABLE"),
        Box::new(|| 123_u64),
    );
    let side = router.add_side_serialized_with_options(
        "cpp",
        tx,
        RouterSideOptions {
            reliable_enabled: true,
            ..Default::default()
        },
    );
    router.rx_serialized_from_side(&hex_decode(hex)?, side)?;
    router.process_tx_queue()?;
    let frames = captured.lock().unwrap();
    if frames.is_empty() {
        return Err(sedsprintf_rs::TelemetryError::Io("rust emitted no ack"));
    }
    let got = values
        .lock()
        .unwrap()
        .clone()
        .ok_or(sedsprintf_rs::TelemetryError::Io("rust received no reliable packet"))?;
    for frame in frames.iter() {
        println!("{}", hex_encode(frame));
    }
    println!("{} {} {}", got[0], got[1], got[2]);
    Ok(())
}

fn reliable_session() -> TelemetryResult<()> {
    let now = Arc::new(AtomicU64::new(123));
    let clock = SharedClock { now: now.clone() };
    let (captured, tx) = capture_side();
    let router = Router::new_with_clock(
        RouterConfig::default().with_sender("RUST_RELIABLE"),
        Box::new(clock),
    );
    let side = router.add_side_serialized_with_options(
        "cpp",
        tx,
        RouterSideOptions {
            reliable_enabled: true,
            ..Default::default()
        },
    );
    router.log_ts(DataType::GpsData, 123, &[61.0_f32, 62.0, 63.0])?;
    {
        let frames = captured.lock().unwrap();
        let data = frames.last().ok_or(sedsprintf_rs::TelemetryError::Io("rust emitted no reliable frame"))?;
        println!("{}", hex_encode(data));
    }

    let mut saw_ack = false;
    for line in io::stdin().lock().lines() {
        let line = line.map_err(|_| sedsprintf_rs::TelemetryError::Io("failed to read ack"))?;
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        router.rx_serialized_from_side(&hex_decode(line)?, side)?;
        saw_ack = true;
    }
    if !saw_ack {
        return Err(sedsprintf_rs::TelemetryError::Io("session received no ack"));
    }

    captured.lock().unwrap().clear();
    now.store(500, Ordering::Relaxed);
    router.process_tx_queue()?;
    if !captured.lock().unwrap().is_empty() {
        return Err(sedsprintf_rs::TelemetryError::Io("rust retransmitted after ack"));
    }
    println!("ACK_ACCEPTED");
    Ok(())
}

fn print_relay_frames(prefix: &str, frames: &[Vec<u8>]) {
    for frame in frames {
        println!("{prefix} {}", hex_encode(frame));
    }
}

fn relay_session() -> TelemetryResult<()> {
    let now = Arc::new(AtomicU64::new(123));
    let clock = SharedClock { now };
    let relay = Relay::new(Box::new(clock));
    let (to_source, source_tx) = capture_side();
    let (to_dest, dest_tx) = capture_side();
    let opts = RelaySideOptions {
        reliable_enabled: true,
        ..Default::default()
    };
    let source = relay.add_side_serialized_with_options("source", source_tx, opts);
    let dest = relay.add_side_serialized_with_options("dest", dest_tx, opts);

    let mut stdin = io::stdin().lock();
    let mut data_hex = String::new();
    stdin
        .read_line(&mut data_hex)
        .map_err(|_| sedsprintf_rs::TelemetryError::Io("relay failed to read data"))?;
    relay.rx_serialized_from_side(source, &hex_decode(data_hex.trim())?)?;
    relay.process_all_queues()?;
    {
        let src = to_source.lock().unwrap();
        let dst = to_dest.lock().unwrap();
        if src.is_empty() || dst.is_empty() {
            return Err(sedsprintf_rs::TelemetryError::Io("relay did not ack and forward data"));
        }
        print_relay_frames("SRC", &src);
        print_relay_frames("DST", &dst);
    }
    println!("END");
    to_source.lock().unwrap().clear();
    to_dest.lock().unwrap().clear();

    for line in stdin.lines() {
        let line = line.map_err(|_| sedsprintf_rs::TelemetryError::Io("relay failed to read ack"))?;
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        relay.rx_serialized_from_side(dest, &hex_decode(line)?)?;
        relay.process_all_queues()?;
    }
    {
        let src = to_source.lock().unwrap();
        let dst = to_dest.lock().unwrap();
        if src.is_empty() {
            return Err(sedsprintf_rs::TelemetryError::Io("relay did not forward acknowledgements"));
        }
        print_relay_frames("SRC", &src);
        print_relay_frames("DST", &dst);
    }
    println!("END");
    Ok(())
}

fn relay_forward(hexes: &[String]) -> TelemetryResult<()> {
    let relay = Relay::new(Box::new(|| 123_u64));
    let (_to_source, source_tx) = capture_side();
    let (to_dest, dest_tx) = capture_side();
    let source = relay.add_side_serialized("source", source_tx);
    relay.add_side_serialized("dest", dest_tx);
    for hex in hexes {
        relay.rx_serialized_from_side(source, &hex_decode(hex)?)?;
        relay.process_all_queues()?;
    }
    let frames = to_dest.lock().unwrap();
    if frames.is_empty() {
        return Err(sedsprintf_rs::TelemetryError::Io("relay forwarded no frames"));
    }
    for frame in frames.iter() {
        println!("{}", hex_encode(frame));
    }
    Ok(())
}

fn emit_discovery() -> TelemetryResult<()> {
    let (captured, tx) = capture_side();
    let handler = EndpointHandler::new_packet_handler(DataEndpoint::Radio, |_pkt| Ok(()));
    let router = Router::new_with_clock(
        RouterConfig::new([handler]).with_sender("RUST_DISC"),
        Box::new(|| 123_u64),
    );
    router.add_side_serialized("cpp", tx);
    router.announce_discovery()?;
    router.process_tx_queue()?;
    for frame in captured.lock().unwrap().iter() {
        println!("{}", hex_encode(frame));
    }
    Ok(())
}

fn consume_discovery(hexes: &[String]) -> TelemetryResult<()> {
    let router = Router::new_with_clock(
        RouterConfig::default().with_sender("RUST_DISC_CONSUMER"),
        Box::new(|| 123_u64),
    );
    let side = router.add_side_serialized("cpp", |_bytes| Ok(()));
    for hex in hexes {
        router.rx_serialized_queue_from_side(&hex_decode(hex)?, side)?;
    }
    router.process_all_queues()?;
    let topo = router.export_topology();
    let saw_radio = topo
        .routes
        .iter()
        .any(|route| route.reachable_endpoints.contains(&DataEndpoint::Radio));
    if !saw_radio {
        return Err(sedsprintf_rs::TelemetryError::Io("rust discovery did not learn radio endpoint"));
    }
    println!("DISCOVERY_OK");
    Ok(())
}

fn source_timesync_config() -> TimeSyncConfig {
    TimeSyncConfig {
        role: TimeSyncRole::Source,
        priority: 10,
        ..Default::default()
    }
}

fn consumer_timesync_config() -> TimeSyncConfig {
    TimeSyncConfig {
        role: TimeSyncRole::Consumer,
        priority: 100,
        ..Default::default()
    }
}

fn emit_timesync() -> TelemetryResult<()> {
    let (captured, tx) = capture_side();
    let router = Router::new_with_clock(
        RouterConfig::default()
            .with_sender("RUST_TIME")
            .with_timesync(source_timesync_config()),
        Box::new(|| 1000_u64),
    );
    router.add_side_serialized("cpp", tx);
    router.set_local_network_datetime(2026, 1, 2, 3, 4, 5);
    router.poll_timesync()?;
    router.process_tx_queue()?;
    for frame in captured.lock().unwrap().iter() {
        println!("{}", hex_encode(frame));
    }
    Ok(())
}

fn consume_timesync(hexes: &[String]) -> TelemetryResult<()> {
    let router = Router::new_with_clock(
        RouterConfig::default()
            .with_sender("RUST_TIME_CONSUMER")
            .with_timesync(consumer_timesync_config()),
        Box::new(|| 1000_u64),
    );
    let side = router.add_side_serialized("cpp", |_bytes| Ok(()));
    for hex in hexes {
        router.rx_serialized_from_side(&hex_decode(hex)?, side)?;
    }
    let network_ms = router
        .network_time_ms()
        .ok_or(sedsprintf_rs::TelemetryError::Io("rust did not learn network time"))?;
    println!("{network_ms}");
    Ok(())
}

fn main() -> TelemetryResult<()> {
    let mut args = std::env::args().skip(1);
    match args.next().as_deref() {
        Some("emit") => emit(),
        Some("consume") => consume(&args.next().ok_or(sedsprintf_rs::TelemetryError::BadArg)?),
        Some("consume-reliable") => consume_reliable(&args.next().ok_or(sedsprintf_rs::TelemetryError::BadArg)?),
        Some("reliable-session") => reliable_session(),
        Some("relay-session") => relay_session(),
        Some("relay-forward") => relay_forward(&args.collect::<Vec<_>>()),
        Some("emit-discovery") => emit_discovery(),
        Some("consume-discovery") => consume_discovery(&args.collect::<Vec<_>>()),
        Some("emit-timesync") => emit_timesync(),
        Some("consume-timesync") => consume_timesync(&args.collect::<Vec<_>>()),
        _ => Err(sedsprintf_rs::TelemetryError::BadArg),
    }
}
'''


def run(cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> str:
    completed = subprocess.run(cmd, cwd=cwd, env=env, text=True, capture_output=True)
    if completed.returncode != 0:
        if completed.stdout:
            print(completed.stdout, end="")
        if completed.stderr:
            print(completed.stderr, end="")
        raise subprocess.CalledProcessError(
            completed.returncode,
            cmd,
            output=completed.stdout,
            stderr=completed.stderr,
        )
    return completed.stdout.strip()


def resolve_rust_root(requested_root: Path, work_dir: Path, repo_url: str) -> Path:
    requested_root = requested_root.expanduser()
    checkout = work_dir / "sedsprintf_rs"

    work_dir.mkdir(parents=True, exist_ok=True)
    if (requested_root / "Cargo.toml").is_file():
        if checkout.exists():
            shutil.rmtree(checkout)

        def ignore(_dir: str, names: list[str]) -> set[str]:
            ignored = {".git", "target", "__pycache__", ".pytest_cache"}
            return {name for name in names if name in ignored}

        shutil.copytree(requested_root, checkout, ignore=ignore)
    else:
        if not (checkout / "Cargo.toml").is_file():
            run(["git", "clone", "--depth", "1", repo_url, str(checkout)])
    return checkout.resolve()


def load_json(path: Path) -> object:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def assert_matching_default_schema(cpp_schema: Path, rust_root: Path) -> None:
    rust_schema = rust_root / "telemetry_config.json"
    if cpp_schema.name != "telemetry_config.json":
        return
    if not rust_schema.is_file():
        raise AssertionError(f"Rust repo has no default schema at {rust_schema}")
    if load_json(cpp_schema) != load_json(rust_schema):
        raise AssertionError(
            f"default telemetry_config.json differs between C++ ({cpp_schema}) and Rust ({rust_schema})"
        )


def rust_build_env(schema_path: Path, ipc_schema_path: Path | None) -> dict[str, str]:
    env = os.environ.copy()
    env["SEDSPRINTF_RS_SCHEMA_PATH"] = str(schema_path.resolve())
    if ipc_schema_path is not None:
        env["SEDSPRINTF_RS_IPC_SCHEMA_PATH"] = str(ipc_schema_path.resolve())
    else:
        env.pop("SEDSPRINTF_RS_IPC_SCHEMA_PATH", None)
    return env


def run_session(cmd: list[str], responder: list[str]) -> None:
    proc = subprocess.Popen(
        cmd,
        text=True,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert proc.stdout is not None
    assert proc.stdin is not None
    data_hex = proc.stdout.readline().strip()
    if not data_hex:
        stderr = proc.stderr.read() if proc.stderr is not None else ""
        raise AssertionError(f"session emitted no data: {stderr}")
    response = run([*responder, data_hex])
    ack_lines = [
        line.strip()
        for line in response.splitlines()
        if line.strip()
        and len(line.strip()) % 2 == 0
        and all(ch in "0123456789abcdefABCDEF" for ch in line.strip())
    ]
    if not ack_lines:
        raise AssertionError(f"responder emitted no serialized ack frames: {response!r}")
    stdout, stderr = proc.communicate(input="\n".join(ack_lines) + "\n", timeout=10)
    if proc.returncode != 0:
        print(stdout, end="")
        print(stderr, end="")
        raise subprocess.CalledProcessError(proc.returncode, cmd, output=stdout, stderr=stderr)
    if "ACK_ACCEPTED" not in stdout:
        raise AssertionError(f"session did not accept ack: {stdout!r}")


def serialized_lines(output: str) -> list[str]:
    return [
        line.strip()
        for line in output.splitlines()
        if line.strip()
        and len(line.strip()) % 2 == 0
        and all(ch in "0123456789abcdefABCDEF" for ch in line.strip())
    ]


def read_relay_phase(proc: subprocess.Popen[str]) -> dict[str, list[str]]:
    assert proc.stdout is not None
    frames: dict[str, list[str]] = {"SRC": [], "DST": []}
    while True:
        line = proc.stdout.readline()
        if not line:
            stderr = proc.stderr.read() if proc.stderr is not None else ""
            raise AssertionError(f"relay ended before phase marker: {stderr}")
        line = line.strip()
        if line == "END":
            return frames
        prefix, _, hex_frame = line.partition(" ")
        if prefix in frames and hex_frame:
            frames[prefix].append(hex_frame)


def run_relay_path(source: list[str], relay: list[str], destination: list[str]) -> None:
    src_proc = subprocess.Popen(
        source,
        text=True,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert src_proc.stdin is not None
    assert src_proc.stdout is not None
    data_hex = src_proc.stdout.readline().strip()
    if not data_hex:
        stderr = src_proc.stderr.read() if src_proc.stderr is not None else ""
        raise AssertionError(f"relay source emitted no data: {stderr}")

    relay_proc = subprocess.Popen(
        relay,
        text=True,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert relay_proc.stdin is not None
    relay_proc.stdin.write(data_hex + "\n")
    relay_proc.stdin.flush()
    first = read_relay_phase(relay_proc)
    if not first["SRC"] or not first["DST"]:
        raise AssertionError(f"relay did not emit source ACK and destination data: {first!r}")

    dest_response = run([*destination, first["DST"][0]])
    dest_acks = serialized_lines(dest_response)
    if not dest_acks:
        raise AssertionError(f"destination emitted no ACK frames: {dest_response!r}")

    relay_proc.stdin.write("\n".join(dest_acks) + "\n")
    relay_proc.stdin.close()
    second = read_relay_phase(relay_proc)
    relay_stderr = relay_proc.stderr.read() if relay_proc.stderr is not None else ""
    if relay_proc.wait(timeout=10) != 0:
        raise subprocess.CalledProcessError(relay_proc.returncode, relay, stderr=relay_stderr)
    if not second["SRC"]:
        raise AssertionError(f"relay did not forward destination ACKs to source: {second!r}")

    source_acks = first["SRC"] + second["SRC"]
    stdout, stderr = src_proc.communicate(input="\n".join(source_acks) + "\n", timeout=10)
    if src_proc.returncode != 0:
        print(stdout, end="")
        print(stderr, end="")
        raise subprocess.CalledProcessError(src_proc.returncode, source, output=stdout, stderr=stderr)
    if "ACK_ACCEPTED" not in stdout:
        raise AssertionError(f"source did not accept relay-routed ACKs: {stdout!r}")


def assert_values(label: str, output: str, expected: tuple[float, float, float]) -> None:
    values = tuple(float(part) for part in output.splitlines()[-1].split())
    if len(values) != len(expected):
        raise AssertionError(f"{label}: expected {len(expected)} values, got {output!r}")
    for got, want in zip(values, expected):
        if abs(got - want) > 0.0001:
            raise AssertionError(f"{label}: expected {expected}, got {values}")


def ensure_rust_harness(work_dir: Path, rust_root: Path) -> Path:
    crate_dir = work_dir / "rust_router_interop"
    src_dir = crate_dir / "src"
    src_dir.mkdir(parents=True, exist_ok=True)
    (crate_dir / "Cargo.toml").write_text(
        textwrap.dedent(
            f"""
            [package]
            name = "rust_router_interop"
            version = "0.0.0"
            edition = "2024"

            [dependencies]
            sedsprintf_rs = {{ path = "{rust_root.as_posix()}" }}
            """
        ).strip()
        + "\n",
        encoding="utf-8",
    )
    (src_dir / "main.rs").write_text(RUST_MAIN, encoding="utf-8")
    return crate_dir


def nonempty_lines(output: str) -> list[str]:
    return [line for line in output.splitlines() if line.strip()]


def assert_nonzero_ms(label: str, output: str) -> None:
    try:
        value = int(output.splitlines()[-1])
    except (IndexError, ValueError) as exc:
        raise AssertionError(f"{label}: invalid network time output {output!r}") from exc
    if value <= 0:
        raise AssertionError(f"{label}: expected nonzero network time, got {value}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpp-peer", required=True, type=Path)
    parser.add_argument("--rust-root", required=True, type=Path)
    parser.add_argument("--rust-repo-url", default=DEFAULT_RUST_REPO_URL)
    parser.add_argument("--schema-path", required=True, type=Path)
    parser.add_argument("--ipc-schema-path", type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()

    rust_root = resolve_rust_root(args.rust_root, args.work_dir, args.rust_repo_url)
    assert_matching_default_schema(args.schema_path.resolve(), rust_root)

    env = rust_build_env(args.schema_path, args.ipc_schema_path)
    crate_dir = ensure_rust_harness(args.work_dir, rust_root)
    cargo = ["cargo", "run", "--quiet", "--manifest-path", str(crate_dir / "Cargo.toml"), "--"]
    cpp = [str(args.cpp_peer)]

    rust_hex = run([*cargo, "emit"], env=env)
    assert_values("C++ receiving Rust frame", run([*cpp, "consume", rust_hex]), (11.25, -2.5, 99.0))

    cpp_hex = run([*cpp, "emit"])
    assert_values("Rust receiving C++ frame", run([*cargo, "consume", cpp_hex], env=env), (41.0, 42.5, -7.25))

    for key in ("SEDSPRINTF_RS_SCHEMA_PATH", "SEDSPRINTF_RS_IPC_SCHEMA_PATH"):
        if key in env:
            os.environ[key] = env[key]
        else:
            os.environ.pop(key, None)
    run_session([*cpp, "reliable-session"], [*cargo, "consume-reliable"])
    run_session([*cargo, "reliable-session"], [*cpp, "consume-reliable"])
    run_relay_path([*cpp, "reliable-session"], [*cargo, "relay-session"], [*cpp, "consume-reliable"])
    run_relay_path([*cargo, "reliable-session"], [*cpp, "relay-session"], [*cpp, "consume-reliable"])

    rust_discovery = nonempty_lines(run([*cargo, "emit-discovery"], env=env))
    if not rust_discovery:
        raise AssertionError("Rust emitted no discovery frames")
    if run([*cpp, "consume-discovery", *rust_discovery]) != "DISCOVERY_OK":
        raise AssertionError("C++ did not accept Rust discovery")
    rust_discovery_via_cpp_relay = nonempty_lines(run([*cpp, "relay-forward", *rust_discovery]))
    if run([*cpp, "consume-discovery", *rust_discovery_via_cpp_relay]) != "DISCOVERY_OK":
        raise AssertionError("C++ relay did not forward Rust discovery")

    cpp_discovery = nonempty_lines(run([*cpp, "emit-discovery"]))
    if not cpp_discovery:
        raise AssertionError("C++ emitted no discovery frames")
    if run([*cargo, "consume-discovery", *cpp_discovery], env=env) != "DISCOVERY_OK":
        raise AssertionError("Rust did not accept C++ discovery")
    cpp_discovery_via_rust_relay = nonempty_lines(run([*cargo, "relay-forward", *cpp_discovery], env=env))
    if run([*cargo, "consume-discovery", *cpp_discovery_via_rust_relay], env=env) != "DISCOVERY_OK":
        raise AssertionError("Rust relay did not forward C++ discovery")

    rust_time = nonempty_lines(run([*cargo, "emit-timesync"], env=env))
    if not rust_time:
        raise AssertionError("Rust emitted no time-sync frames")
    assert_nonzero_ms("C++ consuming Rust time sync", run([*cpp, "consume-timesync", *rust_time]))
    rust_time_via_cpp_relay = nonempty_lines(run([*cpp, "relay-forward", *rust_time]))
    assert_nonzero_ms("C++ consuming Rust time sync through C++ relay", run([*cpp, "consume-timesync", *rust_time_via_cpp_relay]))

    cpp_time = nonempty_lines(run([*cpp, "emit-timesync"]))
    if not cpp_time:
        raise AssertionError("C++ emitted no time-sync frames")
    assert_nonzero_ms("Rust consuming C++ time sync", run([*cargo, "consume-timesync", *cpp_time], env=env))
    cpp_time_via_rust_relay = nonempty_lines(run([*cargo, "relay-forward", *cpp_time], env=env))
    assert_nonzero_ms("Rust consuming C++ time sync through Rust relay", run([*cargo, "consume-timesync", *cpp_time_via_rust_relay], env=env))

    print("Rust/C++ router feature interop passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
