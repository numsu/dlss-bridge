"""Create an isolated launch session without modifying the game or Wine prefix."""
from __future__ import annotations

import json
import os
import shutil
import sys
import uuid
from dataclasses import dataclass
from pathlib import Path

from runtime_setup import model_path, state_root, validate_model


@dataclass(frozen=True)
class LaunchSession:
    identifier: str
    directory: Path
    command: list[str]
    environment: dict[str, str]


def windows_path(path: Path) -> str:
    resolved = path.expanduser().resolve()
    if sys.platform == "win32":
        return str(resolved)
    return "Z:" + str(resolved).replace("/", "\\")


def find_game_executable(command: list[str]) -> tuple[int, Path]:
    candidates: list[tuple[int, Path]] = []
    for index, value in enumerate(command):
        cleaned = value.strip('"')
        if cleaned.lower().endswith(".exe"):
            path = Path(cleaned).expanduser()
            if path.is_file():
                candidates.append((index, path.resolve()))
    if not candidates:
        raise SystemExit(
            "could not find a Windows game executable in the command; "
            "place 'dlss-bridge run --' directly before Steam's %command%"
        )
    return candidates[-1]


def link_or_copy(source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(source, target)
    except OSError:
        shutil.copy2(source, target)


def materialize_tree(source: Path, target: Path) -> None:
    for item in sorted(source.rglob("*")):
        relative = item.relative_to(source)
        destination = target / relative
        if item.is_dir():
            destination.mkdir(parents=True, exist_ok=True)
        elif item.name.lower() == "optiscaler.ini":
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(item, destination)
        elif item.is_file():
            link_or_copy(item, destination)


def parse_runtime_config(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if separator:
            values[key.strip()] = value.strip()
    return values


def configure_neural_runtime(ini: Path, runtime: dict[str, str]) -> None:
    """Apply the public feature policy to the session-local OptiScaler copy."""
    placement = runtime["neural_placement"]
    placement_values = {
        "before_sr": ("true", "false"),
        "after_sr": ("false", "false"),
        "deferred_dlss": ("true", "true"),
    }
    run_before, deferred = placement_values[placement]
    wanted = {
        "RunBeforeSR": run_before,
        "DeferredDLSS": deferred,
        "WorkingScale": runtime["neural_working_scale"],
        "Passes": runtime["neural_passes"],
        # F24 is reserved for the bridge's internal Ctrl+Shift+N translation.
        "ToggleKey": "0x87",
    }
    lines = ini.read_text(encoding="utf-8-sig").splitlines()
    section = ""
    found: set[str] = set()
    for index, original in enumerate(lines):
        stripped = original.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1].strip().lower()
            continue
        if section != "dlssnr" or "=" not in original or stripped.startswith(("#", ";")):
            continue
        key = original.partition("=")[0].strip()
        if key in wanted:
            lines[index] = f"{key}={wanted[key]}"
            found.add(key)
    missing = sorted(set(wanted) - found)
    if missing:
        raise RuntimeError(f"OptiScaler.ini [DlssNr] is missing required settings: {', '.join(missing)}")
    ini.write_text("\n".join(lines) + "\n", encoding="utf-8")


def bridge_runtime_text(runtime: dict[str, str]) -> str:
    optiscaler_keys = {"neural_placement", "neural_working_scale", "neural_passes"}
    return "# generated bridge runtime policy\n" + "".join(
        f"{key}={value}\n" for key, value in runtime.items() if key not in optiscaler_keys
    )


def prepare_session(
    payload: Path,
    command: list[str],
    config_text: str,
    environment: dict[str, str] | None = None,
) -> LaunchSession:
    game_index, game = find_game_executable(command)
    bridge = payload / "bridge"
    optiscaler = payload / "optiscaler"
    launcher = bridge / "dlss-bridge-launcher.exe"
    hook = bridge / "dlss5-vk-hook.dll"
    model = model_path()
    required = [launcher, hook, optiscaler / "OptiScaler.dll",
                optiscaler / "nvngx.dll_dlssnr.dll", model]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        hint = "\nReinstall DLSS Bridge to restore the neural runtime." if str(model) in missing else ""
        raise SystemExit("missing runtime files:\n  " + "\n  ".join(missing) + hint)
    validate_model(model)

    identifier = uuid.uuid4().hex
    directory = state_root() / "sessions" / identifier
    directory.mkdir(parents=True, exist_ok=False)
    try:
        materialize_tree(optiscaler, directory)
        runtime = parse_runtime_config(config_text)
        configure_neural_runtime(directory / "OptiScaler.ini", runtime)
        link_or_copy(hook, directory / hook.name)
        link_or_copy(launcher, directory / launcher.name)
        link_or_copy(model, directory / model.name)
        config = directory / "dlss5-vk-bridge.cfg"
        config.write_text(bridge_runtime_text(runtime))

        rewritten = list(command)
        rewritten[game_index:game_index + 1] = [
            str(directory / launcher.name),
            "--hook",
            windows_path(directory / hook.name),
            "--",
            windows_path(game),
        ]
        env = dict(environment if environment is not None else os.environ)
        env["DLSS_BRIDGE_CONFIG"] = windows_path(config)
        env["DLSS_BRIDGE_SESSION"] = windows_path(directory)
        manifest = {
            "schema": 1,
            "session": identifier,
            "game": str(game),
            "command": rewritten,
        }
        (directory / "session.json").write_text(json.dumps(manifest, indent=2) + "\n")
        return LaunchSession(identifier, directory, rewritten, env)
    except BaseException:
        shutil.rmtree(directory, ignore_errors=True)
        raise


def close_session(session: LaunchSession) -> None:
    logs = state_root() / "logs" / session.identifier
    copied = False
    for source in session.directory.rglob("*.log"):
        if source.is_file():
            destination = logs / source.relative_to(session.directory)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
            copied = True
    if not copied and logs.is_dir():
        shutil.rmtree(logs, ignore_errors=True)
    shutil.rmtree(session.directory, ignore_errors=False)
