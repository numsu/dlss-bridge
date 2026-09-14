#!/usr/bin/env python3
"""Portable controller for DLSS Bridge.

Configuration stays in the installation, launch resources live in a disposable
user-state session, and game directories and Wine prefixes remain untouched.
Frame data remains in-process.
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tomllib
from typing import Any

_controller_dir = str(Path(__file__).resolve().parent)
if _controller_dir not in sys.path:
    sys.path.insert(0, _controller_dir)
from runtime_setup import acquire_model, import_model, model_path, state_root, validate_model
from session import close_session, prepare_session
from steam_config import (bridge_command, configure as configure_steam,
                          remove as remove_steam, select_localconfig, steam_running)

ROOT = (Path(sys.executable).resolve().parent if getattr(sys, "frozen", False)
        else Path(__file__).resolve().parent.parent)
PAYLOAD = ROOT / "payload"


def inside_root(path: Path) -> Path:
    resolved = path.expanduser().resolve()
    try:
        resolved.relative_to(ROOT)
    except ValueError as exc:
        raise SystemExit(f"refusing to write outside project root {ROOT}: {resolved}") from exc
    return resolved


def load_toml(path: Path) -> dict[str, Any]:
    with path.open("rb") as stream:
        return tomllib.load(stream)


def profile_path(value: str) -> Path:
    direct = Path(value).expanduser()
    if direct.is_file():
        return direct.resolve()
    bundled = ROOT / "profiles" / value
    if bundled.is_file():
        return bundled
    with_suffix = bundled if bundled.suffix else bundled.with_suffix(".toml")
    if with_suffix.is_file():
        return with_suffix
    raise SystemExit(f"profile does not exist: {value}")


def merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
    result = dict(base)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(result.get(key), dict):
            result[key] = merge(result[key], value)
        else:
            result[key] = value
    return result


def bool_int(value: Any) -> int:
    if isinstance(value, bool):
        return int(value)
    if value in (0, 1):
        return int(value)
    raise SystemExit(f"expected boolean, got {value!r}")


PROFILE_KEYS = {
    "feature": {"kind", "placement", "working_scale", "passes"},
    "execution": {"mode", "compute_adapter"},
    "transport": {"output_format", "neural_queue", "ring_slots",
                  "neural_pipeline_frames", "latency_budget_ms"},
    "telemetry": {"gpu_timestamps"},
    "bridge": {"verbose"},
    "match": {"executable", "graphics_api"},
    "capture": {"adapter"},
}


def validate_profile(data: dict[str, Any], source: str = "profile") -> None:
    if data.get("schema") != 1:
        raise SystemExit(f"{source}: schema must be 1")
    unknown_sections = set(data) - {"schema", *PROFILE_KEYS}
    if unknown_sections:
        raise SystemExit(f"{source}: unknown section(s): {', '.join(sorted(unknown_sections))}")
    for section, allowed in PROFILE_KEYS.items():
        value = data.get(section, {})
        if not isinstance(value, dict):
            raise SystemExit(f"{source}: [{section}] must be a table")
        unknown = set(value) - allowed
        if unknown:
            raise SystemExit(f"{source}: unknown [{section}] setting(s): {', '.join(sorted(unknown))}")
    kind = data.get("feature", {}).get("kind", "neural_rendering")
    if kind != "neural_rendering":
        raise SystemExit(f"{source}: unsupported feature.kind: {kind}")
    adapter = data.get("capture", {}).get("adapter", "ngx_vulkan")
    if adapter != "ngx_vulkan":
        raise SystemExit(f"{source}: capture.adapter is not integrated: {adapter}")
    graphics_api = data.get("match", {}).get("graphics_api", "vulkan")
    if graphics_api != "vulkan":
        raise SystemExit(f"{source}: unsupported match.graphics_api: {graphics_api}")


def resolved_runtime(data: dict[str, Any]) -> dict[str, str | int]:
    validate_profile(data)
    feature = data.get("feature", {})
    execution = data.get("execution", {})
    transport = data.get("transport", {})
    bridge = data.get("bridge", {})
    placement = str(feature.get("placement", "after_sr"))
    if placement not in ("after_sr", "before_sr", "deferred_dlss"):
        raise SystemExit(f"invalid feature.placement: {placement}")
    working_scale = float(feature.get("working_scale", 1.0))
    if not math.isfinite(working_scale) or not 0.25 <= working_scale <= 2.0:
        raise SystemExit("feature.working_scale must be between 0.25 and 2.0")
    passes = int(feature.get("passes", 1))
    if not 1 <= passes <= 3:
        raise SystemExit("feature.passes must be between 1 and 3")
    telemetry = data.get("telemetry", {})
    mode = execution.get("mode", "auto")
    if mode not in ("auto", "same_gpu", "secondary_gpu"):
        raise SystemExit(f"invalid execution.mode: {mode}")
    selector = str(execution.get("compute_adapter", "auto"))
    if not (selector in ("auto", "game") or selector.startswith(("index:", "luid:"))):
        raise SystemExit(f"invalid execution.compute_adapter: {selector}")
    slots = int(transport.get("ring_slots", 3))
    if not 2 <= slots <= 8:
        raise SystemExit("transport.ring_slots must be between 2 and 8")
    pipeline_frames = int(transport.get("neural_pipeline_frames", 2))
    if not 1 <= pipeline_frames < slots:
        raise SystemExit("transport.neural_pipeline_frames must be at least 1 and smaller than ring_slots")
    budget = int(transport.get("latency_budget_ms", 16))
    output_transport = str(transport.get("output_format", "native"))
    if output_transport not in ("native", "r11g11b10_float"):
        raise SystemExit(f"invalid transport.output_format: {output_transport}")
    neural_queue_mode = str(transport.get("neural_queue", "split"))
    if neural_queue_mode not in ("split", "unified"):
        raise SystemExit(f"invalid transport.neural_queue: {neural_queue_mode}")
    if not 1 <= budget <= 125:
        raise SystemExit("transport.latency_budget_ms must be between 1 and 125")
    return {
        "verbose": bool_int(bridge.get("verbose", False)),
        "execution_mode": mode,
        "compute_adapter": selector,
        "ring_slots": slots,
        "neural_pipeline_frames": pipeline_frames,
        "latency_budget_ms": budget,
        "output_transport": output_transport,
        "neural_queue_mode": neural_queue_mode,
        "neural_placement": placement,
        "neural_working_scale": format(working_scale, ".6g"),
        "neural_passes": passes,
        "gpu_timestamps": bool_int(telemetry.get("gpu_timestamps", True)),
    }


def runtime_text(settings: dict[str, str | int]) -> str:
    body = "# generated by controller/dlss_bridge.py; neural-only policy\n"
    return body + "".join(f"{key}={value}\n" for key, value in settings.items())


def write_runtime(settings: dict[str, str | int], output: Path) -> None:
    output = inside_root(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(runtime_text(settings))


def cmd_resolve(args: argparse.Namespace) -> int:
    data = load_toml(Path(args.config))
    validate_profile(data, str(args.config))
    for profile in args.profile:
        path = profile_path(profile)
        item = load_toml(path)
        validate_profile(item, str(path))
        data = merge(data, item)
    write_runtime(resolved_runtime(data), Path(args.output))
    print(Path(args.output).expanduser().resolve())
    return 0


def gpu_inventory() -> list[dict[str, Any]]:
    fields = ["index", "uuid", "pci.bus_id", "name", "driver_version"]
    command = ["nvidia-smi", f"--query-gpu={','.join(fields)}", "--format=csv,noheader,nounits"]
    try:
        completed = subprocess.run(command, text=True, capture_output=True, check=True)
    except (FileNotFoundError, subprocess.CalledProcessError) as exc:
        return [{"available": False, "error": str(exc)}]
    rows = []
    for line in completed.stdout.splitlines():
        values = [part.strip() for part in line.split(",", len(fields) - 1)]
        if len(values) == len(fields):
            rows.append(dict(zip(fields, values)) | {"available": True})
    return rows


def component_inventory() -> list[dict[str, Any]]:
    components: list[dict[str, Any]] = []
    for group in ("hosts", "capture", "executors", "transports", "platform"):
        for manifest in sorted((ROOT / group).glob("*/backend.toml")):
            item = load_toml(manifest)
            for required in ("schema", "kind", "id", "state"):
                if required not in item:
                    raise SystemExit(f"{manifest}: missing {required}")
            if item["state"] == "integrated" and item["kind"] != "platform":
                if item.get("linkage") != "static" or item.get("abi") != 1:
                    raise SystemExit(f"{manifest}: integrated component must declare static ABI 1 linkage")
                implementation = item.get("implementation")
                if not implementation or not (ROOT / implementation).is_file():
                    raise SystemExit(f"{manifest}: integrated component implementation is missing")
            item["manifest"] = str(manifest.relative_to(ROOT))
            components.append(item)
    return components


def cmd_components(args: argparse.Namespace) -> int:
    print(json.dumps({"schema": 1, "components": component_inventory()}, indent=2))
    return 0


def cmd_probe(args: argparse.Namespace) -> int:
    runtime = {"available": False}
    try:
        path = model_path()
        validate_model(path)
        runtime = {"available": True, "path": str(path)}
    except (FileNotFoundError, RuntimeError, ValueError, SystemExit) as exc:
        runtime = {"available": False, "error": str(exc)}
    report = {
        "schema": 1,
        "controller": "portable-dlss-bridge",
        "platform": sys.platform,
        "gpus": gpu_inventory(),
        "neural_runtime": runtime,
        "scope": "installation and hardware inventory; game feature and transport validation occurs at launch",
    }
    encoded = json.dumps(report, indent=2) + "\n"
    if args.output:
        output = inside_root(Path(args.output))
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(encoded)
    print(encoded, end="")
    return 0


def command_executable_name(command: list[str]) -> str | None:
    names = [Path(value.strip('"')).name.lower() for value in command
             if value.strip('"').lower().endswith(".exe")]
    return names[-1] if names else None


def matching_profiles(command: list[str]) -> list[Path]:
    executable = command_executable_name(command)
    if not executable:
        return []
    matched: list[Path] = []
    for path in sorted((ROOT / "profiles").glob("*.toml")):
        item = load_toml(path)
        validate_profile(item, str(path))
        wanted = item.get("match", {}).get("executable")
        if wanted and Path(str(wanted)).name.lower() == executable:
            matched.append(path)
    return matched


def resolve_from_args(args: argparse.Namespace) -> dict[str, str | int]:
    data = load_toml(Path(args.config))
    validate_profile(data, str(args.config))
    for path in matching_profiles(args.command):
        data = merge(data, load_toml(path))
    for profile in args.profile:
        path = profile_path(profile)
        item = load_toml(path)
        validate_profile(item, str(path))
        data = merge(data, item)
    return resolved_runtime(data)


def cmd_import_model(args: argparse.Namespace) -> int:
    result = import_model(Path(args.path))
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(f"Neural runtime imported: {result['installed']}")
    return 0


def cmd_acquire_runtime(args: argparse.Namespace) -> int:
    result = acquire_model(ROOT / "runtime-sources.json", args.force)
    if args.json:
        print(json.dumps(result, indent=2))
        return 0
    messages = {
        "installed": "Neural runtime downloaded and verified.",
        "updated": "Neural runtime updated and verified.",
        "current": "Neural runtime is already installed and verified.",
        "preserved-user-runtime": "Existing user-provided neural runtime preserved.",
    }
    print(messages.get(result["status"], "Neural runtime is ready."))
    return 0


def steam_appid(value: str) -> str:
    if not value.isdecimal() or int(value) <= 0:
        raise argparse.ArgumentTypeError("AppID must be a positive integer")
    return str(int(value))


def steam_config_path(args: argparse.Namespace) -> Path:
    root = Path(args.steam_root) if args.steam_root else None
    return select_localconfig(root, args.user)


def require_steam_stopped() -> None:
    if steam_running():
        raise SystemExit("Steam is running. Exit Steam completely, then run this command again.")


def cmd_steam_configure(args: argparse.Namespace) -> int:
    require_steam_stopped()
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", args.profile):
        raise SystemExit("Steam configuration requires a bundled profile name")
    if args.profile != "default":
        profile_path(args.profile)
    config = steam_config_path(args)
    option = bridge_command(ROOT, args.profile)
    result = configure_steam(config, args.appid, option, state_root())
    verb = "Already configured" if result == "unchanged" else "Configured"
    print(f"{verb} Steam AppID {args.appid}: {option}")
    return 0


def cmd_steam_remove(args: argparse.Namespace) -> int:
    require_steam_stopped()
    config = steam_config_path(args)
    remove_steam(config, args.appid, state_root())
    print(f"Restored the previous launch options for Steam AppID {args.appid}.")
    return 0


def cmd_launch(args: argparse.Namespace) -> int:
    if not args.command:
        raise SystemExit("launch requires a command after --")
    settings = resolve_from_args(args)
    session = prepare_session(
        PAYLOAD, args.command, runtime_text(settings), os.environ.copy()
    )
    try:
        if args.dry_run:
            print(json.dumps({
                "session": str(session.directory),
                "command": session.command,
                "game_files_modified": False,
            }, indent=2))
            return 0
        return subprocess.run(session.command, env=session.environment, check=False).returncode
    finally:
        close_session(session)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    commands = result.add_subparsers(dest="subcommand", required=True)
    resolve = commands.add_parser("resolve")
    resolve.add_argument("--config", default=str(ROOT / "profiles" / "default.toml"))
    resolve.add_argument("--profile", action="append", default=[])
    resolve.add_argument("--output", required=True)
    resolve.set_defaults(func=cmd_resolve)
    components = commands.add_parser("components")
    components.set_defaults(func=cmd_components)
    probe = commands.add_parser("probe")
    probe.add_argument("--output")
    probe.set_defaults(func=cmd_probe)
    model = commands.add_parser("import-model", help="install a user-supplied NVIDIA neural runtime")
    model.add_argument("path")
    model.add_argument("--json", action="store_true", help="print a machine-readable result")
    model.set_defaults(func=cmd_import_model)
    acquire = commands.add_parser(
        "acquire-runtime", help="download and verify the pinned NVIDIA neural runtime"
    )
    acquire.add_argument("--force", action="store_true",
                         help="replace an existing user-supplied runtime")
    acquire.add_argument("--json", action="store_true", help="print a machine-readable result")
    acquire.set_defaults(func=cmd_acquire_runtime)

    steam = commands.add_parser("steam", help="manage Steam launch options")
    steam_commands = steam.add_subparsers(dest="steam_command", required=True)
    steam_configure = steam_commands.add_parser(
        "configure", help="attach DLSS Bridge to a Steam game"
    )
    steam_configure.add_argument("appid", type=steam_appid)
    steam_configure.add_argument("--profile", default="default")
    steam_configure.add_argument("--steam-root")
    steam_configure.add_argument("--user")
    steam_configure.set_defaults(func=cmd_steam_configure)
    steam_remove = steam_commands.add_parser(
        "remove", help="restore a game's previous Steam launch options"
    )
    steam_remove.add_argument("appid", type=steam_appid)
    steam_remove.add_argument("--steam-root")
    steam_remove.add_argument("--user")
    steam_remove.set_defaults(func=cmd_steam_remove)

    launch = commands.add_parser("launch", aliases=["run"])
    launch.add_argument("--config", default=str(ROOT / "profiles" / "default.toml"))
    launch.add_argument("--profile", action="append", default=[])
    launch.add_argument("--dry-run", action="store_true")
    launch.add_argument("command", nargs=argparse.REMAINDER)
    launch.set_defaults(func=cmd_launch)
    return result


if __name__ == "__main__":
    parsed = parser().parse_args()
    if parsed.subcommand in ("launch", "run") and parsed.command[:1] == ["--"]:
        parsed.command = parsed.command[1:]
    raise SystemExit(parsed.func(parsed))
