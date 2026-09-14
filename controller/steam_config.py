"""Safely manage Steam per-game launch options."""
from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from typing import Iterable


@dataclass(frozen=True)
class Token:
    kind: str
    value: str
    start: int
    end: int


@dataclass
class ObjectNode:
    open_token: Token
    close_token: Token
    entries: dict[str, tuple[Token, Token | "ObjectNode"]]


def _tokens(text: str) -> list[Token]:
    result: list[Token] = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if char.isspace():
            index += 1
            continue
        if text.startswith("//", index):
            newline = text.find("\n", index + 2)
            index = length if newline < 0 else newline + 1
            continue
        if char in "{}":
            result.append(Token(char, char, index, index + 1))
            index += 1
            continue
        if char != '"':
            raise ValueError(f"unexpected VDF character at byte {index}: {char!r}")
        start = index
        index += 1
        value: list[str] = []
        while index < length:
            char = text[index]
            if char == '"':
                index += 1
                result.append(Token("string", "".join(value), start, index))
                break
            if char == "\\":
                index += 1
                if index >= length:
                    raise ValueError("unterminated VDF escape")
                escaped = text[index]
                value.append({"n": "\n", "r": "\r", "t": "\t"}.get(escaped, escaped))
                index += 1
                continue
            value.append(char)
            index += 1
        else:
            raise ValueError("unterminated VDF string")
    return result


def _parse_object(tokens: list[Token], index: int) -> tuple[ObjectNode, int]:
    if index >= len(tokens) or tokens[index].kind != "{":
        raise ValueError("expected VDF object")
    opened = tokens[index]
    index += 1
    entries: dict[str, tuple[Token, Token | ObjectNode]] = {}
    while index < len(tokens) and tokens[index].kind != "}":
        key = tokens[index]
        if key.kind != "string":
            raise ValueError(f"expected VDF key at byte {key.start}")
        index += 1
        if index >= len(tokens):
            raise ValueError(f"missing value for VDF key {key.value!r}")
        if tokens[index].kind == "{":
            value, index = _parse_object(tokens, index)
        elif tokens[index].kind == "string":
            value = tokens[index]
            index += 1
        else:
            raise ValueError(f"invalid value for VDF key {key.value!r}")
        entries[key.value.casefold()] = (key, value)
    if index >= len(tokens):
        raise ValueError("unterminated VDF object")
    return ObjectNode(opened, tokens[index], entries), index + 1


def _parse_root(text: str) -> ObjectNode:
    tokens = _tokens(text)
    synthetic = Token("{", "{", 0, 0)
    closing = Token("}", "}", len(text), len(text))
    entries: dict[str, tuple[Token, Token | ObjectNode]] = {}
    index = 0
    while index < len(tokens):
        key = tokens[index]
        if key.kind != "string":
            raise ValueError(f"expected top-level VDF key at byte {key.start}")
        index += 1
        if index >= len(tokens):
            raise ValueError(f"missing value for VDF key {key.value!r}")
        if tokens[index].kind == "{":
            value, index = _parse_object(tokens, index)
        elif tokens[index].kind == "string":
            value = tokens[index]
            index += 1
        else:
            raise ValueError(f"invalid value for VDF key {key.value!r}")
        entries[key.value.casefold()] = (key, value)
    return ObjectNode(synthetic, closing, entries)


def _child(node: ObjectNode, key: str) -> ObjectNode:
    entry = node.entries.get(key.casefold())
    if not entry or not isinstance(entry[1], ObjectNode):
        raise KeyError(key)
    return entry[1]


def _apps(root: ObjectNode) -> ObjectNode:
    node = root
    for key in ("UserLocalConfigStore", "Software", "Valve", "Steam", "apps"):
        node = _child(node, key)
    return node


def _quoted(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    return f'"{escaped}"'


def _line_bounds(text: str, start: int, end: int) -> tuple[int, int]:
    line_start = text.rfind("\n", 0, start) + 1
    line_end = text.find("\n", end)
    if line_end < 0:
        line_end = len(text)
    else:
        line_end += 1
    return line_start, line_end


def read_launch_option(text: str, appid: str) -> str | None:
    app = _child(_apps(_parse_root(text)), appid)
    entry = app.entries.get("launchoptions")
    if not entry:
        return None
    value = entry[1]
    if isinstance(value, ObjectNode):
        raise ValueError("LaunchOptions is unexpectedly an object")
    return value.value


def set_launch_option(text: str, appid: str, option: str | None) -> str:
    app = _child(_apps(_parse_root(text)), appid)
    entry = app.entries.get("launchoptions")
    if entry:
        key, value = entry
        if isinstance(value, ObjectNode):
            raise ValueError("LaunchOptions is unexpectedly an object")
        if option is not None:
            return text[:value.start] + _quoted(option) + text[value.end:]
        start, end = _line_bounds(text, key.start, value.end)
        return text[:start] + text[end:]
    if option is None:
        return text
    close = app.close_token
    line_start = text.rfind("\n", 0, close.start) + 1
    indent = text[line_start:close.start]
    if indent.strip():
        indent = ""
        insertion_at = close.start
        prefix = "\n"
    else:
        insertion_at = line_start
        prefix = ""
    child_indent = indent + "\t"
    insertion = f'{prefix}{child_indent}"LaunchOptions"\t\t{_quoted(option)}\n'
    return text[:insertion_at] + insertion + text[insertion_at:]


def steam_running() -> bool:
    if os.name == "nt":
        try:
            output = subprocess.run(
                ["tasklist", "/FI", "IMAGENAME eq steam.exe", "/FO", "CSV", "/NH"],
                check=False, capture_output=True, text=True,
            ).stdout.casefold()
            return "steam.exe" in output
        except OSError:
            return False
    proc = Path("/proc")
    if not proc.is_dir():
        return False
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if (entry / "comm").read_text().strip().casefold() == "steam":
                return True
        except OSError:
            pass
    return False


def steam_roots(explicit: Path | None = None) -> list[Path]:
    if explicit:
        return [explicit.expanduser().resolve()]
    home = Path.home()
    candidates: list[Path] = []
    if os.name == "nt":
        try:
            import winreg
            for hive in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
                try:
                    with winreg.OpenKey(hive, r"Software\Valve\Steam") as key:
                        candidates.append(Path(winreg.QueryValueEx(key, "SteamPath")[0]))
                except OSError:
                    pass
        except ImportError:
            pass
        program_files = os.environ.get("PROGRAMFILES(X86)") or os.environ.get("PROGRAMFILES")
        if program_files:
            candidates.append(Path(program_files) / "Steam")
    else:
        candidates.extend([
            home / ".steam" / "debian-installation",
            home / ".steam" / "steam",
            home / ".local" / "share" / "Steam",
            home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam",
        ])
    unique: list[Path] = []
    seen: set[Path] = set()
    for candidate in candidates:
        try:
            resolved = candidate.expanduser().resolve()
        except OSError:
            continue
        if resolved.is_dir() and resolved not in seen:
            seen.add(resolved)
            unique.append(resolved)
    return unique


def localconfigs(explicit_root: Path | None = None, user: str | None = None) -> list[Path]:
    found: list[Path] = []
    for root in steam_roots(explicit_root):
        userdata = root / "userdata"
        if not userdata.is_dir():
            continue
        users: Iterable[Path] = [userdata / user] if user else userdata.iterdir()
        for directory in users:
            path = directory / "config" / "localconfig.vdf"
            if directory.name.isdigit() and path.is_file():
                found.append(path.resolve())
    return sorted(set(found))


def select_localconfig(explicit_root: Path | None = None, user: str | None = None) -> Path:
    paths = localconfigs(explicit_root, user)
    if not paths:
        raise SystemExit("could not find a Steam user localconfig.vdf")
    if len(paths) > 1:
        users = ", ".join(path.parents[1].name for path in paths)
        raise SystemExit(f"multiple Steam users found ({users}); select one with --user")
    return paths[0]


def bridge_command(root: Path, profile: str) -> str:
    if getattr(sys, "frozen", False):
        executable = Path(sys.executable)
    else:
        executable = root.parent.parent / "bin" / "dlss-bridge"
        if not executable.is_file():
            located = shutil.which("dlss-bridge")
            if not located:
                raise SystemExit("could not locate the installed dlss-bridge command")
            executable = Path(located)
    rendered = str(executable)
    if any(char.isspace() for char in rendered):
        rendered = f'"{rendered}"'
    profile_option = "" if profile == "default" else f" --profile {profile}"
    return f"{rendered} run{profile_option} -- %command%"


def _records_path(state: Path) -> Path:
    return state / "steam-launch-options.json"


def _load_records(state: Path) -> dict[str, object]:
    path = _records_path(state)
    if not path.is_file():
        return {"schema": 1, "games": {}}
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"invalid Steam launch-option state {path}: {exc}") from exc
    if data.get("schema") != 1 or not isinstance(data.get("games"), dict):
        raise SystemExit(f"invalid Steam launch-option state {path}")
    return data


def _atomic_write(path: Path, text: str) -> None:
    mode = path.stat().st_mode
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w") as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _save_records(state: Path, records: dict[str, object]) -> None:
    state.mkdir(parents=True, exist_ok=True)
    path = _records_path(state)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(records, indent=2) + "\n")
    os.replace(temporary, path)


def _key(config: Path, appid: str) -> str:
    return f"{config.parents[1].name}:{appid}"


def configure(config: Path, appid: str, option: str, state: Path) -> str:
    text = config.read_text()
    try:
        previous = read_launch_option(text, appid)
    except KeyError as exc:
        raise SystemExit(f"Steam AppID {appid} is not present in {config}") from exc
    records = _load_records(state)
    games = records["games"]
    assert isinstance(games, dict)
    key = _key(config, appid)
    if key not in games:
        games[key] = {"previous": previous, "configured": option}
    else:
        entry = games[key]
        if not isinstance(entry, dict):
            raise SystemExit("saved Steam launch option is invalid")
        if previous != entry.get("configured"):
            raise SystemExit(
                "launch options changed after DLSS Bridge configured them; refusing to overwrite"
            )
        entry["configured"] = option
    if previous == option:
        _save_records(state, records)
        return "unchanged"
    backup = config.with_name(config.name + ".dlss-bridge-backup")
    if not backup.exists():
        shutil.copy2(config, backup)
    _atomic_write(config, set_launch_option(text, appid, option))
    _save_records(state, records)
    return "configured"


def remove(config: Path, appid: str, state: Path) -> str:
    records = _load_records(state)
    games = records["games"]
    assert isinstance(games, dict)
    key = _key(config, appid)
    entry = games.get(key)
    if not isinstance(entry, dict):
        raise SystemExit(f"DLSS Bridge has no saved launch option for Steam AppID {appid}")
    text = config.read_text()
    try:
        current = read_launch_option(text, appid)
    except KeyError as exc:
        raise SystemExit(f"Steam AppID {appid} is not present in {config}") from exc
    configured = entry.get("configured")
    if current != configured:
        raise SystemExit("launch options changed after DLSS Bridge configured them; refusing to overwrite")
    previous = entry.get("previous")
    if previous is not None and not isinstance(previous, str):
        raise SystemExit("saved Steam launch option is invalid")
    _atomic_write(config, set_launch_option(text, appid, previous))
    del games[key]
    _save_records(state, records)
    return "removed"
