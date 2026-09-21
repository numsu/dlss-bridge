"""Safely manage Heroic Games Launcher per-game wrapper options.

Heroic builds each game's launch command as gamescope, the configured
``wrapperOptions`` entries (``{exe, args}`` tokens, in order), MangoHud,
GameMode, the Steam runtime, then the game itself. Pointing one wrapper entry
at ``dlss-bridge run --`` therefore prefixes the game exactly like Steam's
``%command%`` placeholder does. Other wrappers after ours (MangoHud,
GameMode) keep working through wrapper chaining: the controller finds the
last ``.exe`` in the composed command and replaces only that token.

Per-game settings live in ``<config>/GamesConfig/<AppName>.json`` as partial
JSON merged over Heroic's defaults, so creating a minimal file holding only
``wrapperOptions`` is safe. Exit Heroic before changing anything: its store
keeps settings in memory and would silently drop external edits.

Configuring a game also ensures Proton exposes NVIDIA GPUs for NGX/DLSS
detection (``PROTON_ENABLE_NVAPI=1``, ``PROTON_HIDE_NVIDIA_GPU=0`` in
``enviromentOptions`` — Heroic's spelling). Only those two keys are ever
touched; every other environment entry is preserved key-for-key.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from steam_config import bridge_executable_path


# Proton must expose NVIDIA APIs or DX11/DX12 NGX titles hide their DLSS
# option (game sees an AMD card, no nvngx deploy). Managed on every
# configure so the end user needs no manual env step.
MANAGED_ENV: dict[str, str] = {
    "PROTON_ENABLE_NVAPI": "1",
    "PROTON_HIDE_NVIDIA_GPU": "0",
}
MANAGED_ENV_ORDER: tuple[str, ...] = (
    "PROTON_ENABLE_NVAPI",
    "PROTON_HIDE_NVIDIA_GPU",
)


def heroic_running() -> bool:
    if os.name == "nt":
        try:
            output = subprocess.run(
                ["tasklist", "/FI", "IMAGENAME eq Heroic.exe", "/FO", "CSV", "/NH"],
                check=False, capture_output=True, text=True,
            ).stdout.casefold()
            return "heroic.exe" in output
        except OSError:
            return False
    proc = Path("/proc")
    if not proc.is_dir():
        return False
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if (entry / "comm").read_text().strip().casefold() == "heroic":
                return True
        except OSError:
            pass
    return False


def heroic_roots(explicit: Path | None = None) -> list[Path]:
    if explicit:
        return [explicit.expanduser().resolve()]
    home = Path.home()
    candidates: list[Path] = []
    xdg = os.environ.get("XDG_CONFIG_HOME")
    if xdg:
        candidates.append(Path(xdg) / "heroic")
    candidates.append(home / ".config" / "heroic")
    candidates.append(
        home / ".var" / "app" / "com.heroicgameslauncher.hgl"
        / "config" / "heroic"
    )
    unique: list[Path] = []
    seen: set[Path] = set()
    for candidate in candidates:
        try:
            resolved = candidate.expanduser().resolve()
        except OSError:
            continue
        # A fresh Heroic install has <root>/config.json but no GamesConfig/
        # until per-game settings are saved; accept the root itself so list
        # reports empty and configure can create GamesConfig/.
        if resolved.is_dir() and resolved not in seen:
            seen.add(resolved)
            unique.append(resolved)
    return unique


def select_games_config(explicit_root: Path | None = None) -> Path:
    if explicit_root is not None:
        root = explicit_root.expanduser().resolve()
        if not root.is_dir():
            raise SystemExit(f"no Heroic configuration under {explicit_root}")
        return root / "GamesConfig"
    roots = heroic_roots(None)
    if not roots:
        home = Path.home()
        xdg = os.environ.get("XDG_CONFIG_HOME")
        searched: list[str] = []
        if xdg:
            searched.append(str(Path(xdg) / "heroic" / "GamesConfig"))
        searched.append(str(home / ".config" / "heroic" / "GamesConfig"))
        searched.append(str(
            home / ".var" / "app" / "com.heroicgameslauncher.hgl"
            / "config" / "heroic" / "GamesConfig"))
        raise SystemExit(
            "no Heroic configuration found "
            f"(HOME={home}, XDG_CONFIG_HOME={xdg or '(unset)'}; looked for "
            f"{', '.join(searched)}). If running inside the "
            "headless-sunshine-steam container, re-run as the gamer user "
            "(`docker exec --user gamer ...`; plain `docker exec` defaults "
            "to root with HOME=/root) or pass --heroic-root."
        )
    if len(roots) > 1:
        found = ", ".join(str(path) for path in roots)
        raise SystemExit(
            f"multiple Heroic configurations found ({found}); select one with --heroic-root"
        )
    return roots[0] / "GamesConfig"


def list_games(games: Path) -> list[str]:
    """App names with a per-game settings file (the configure/remove argument)."""
    if not games.is_dir():
        return []
    return sorted(path.stem for path in games.glob("*.json") if path.is_file())


def _titles_from_library_doc(doc: object) -> dict[str, str]:
    """Extract {app_name: title} from a Heroic library cache document.

    Handles the observed Epic schema (``{"library": [{app_name, title}]}``)
    and tolerates GOG/Nile variants that store games as a dict or a bare
    list; unknown shapes yield no titles rather than failing.
    """
    titles: dict[str, str] = {}
    entries: list[object] = []
    if isinstance(doc, dict):
        library = doc.get("library")
        if isinstance(library, list):
            entries = library
        else:
            for key, value in doc.items():
                if key.startswith("_") or key in ("library", "__timestamp"):
                    continue
                if isinstance(value, dict):
                    title = value.get("title", value.get("name"))
                    if isinstance(key, str) and isinstance(title, str) and title:
                        titles.setdefault(key, title)
            return titles
    elif isinstance(doc, list):
        entries = doc
    else:
        return titles
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        app = entry.get("app_name", entry.get("appName", entry.get("id")))
        title = entry.get("title", entry.get("name"))
        if isinstance(app, str) and isinstance(title, str) and app and title:
            titles.setdefault(app, title)
    return titles


def _titles_from_install_info(doc: object) -> dict[str, str]:
    """Extract {app_name: title} from a Heroic install-info cache document."""
    titles: dict[str, str] = {}
    if not isinstance(doc, dict):
        return titles
    for key, value in doc.items():
        if key.startswith("_") or key in ("__timestamp",):
            continue
        if not isinstance(value, dict):
            continue
        game = value.get("game")
        node = game if isinstance(game, dict) else value
        title = node.get("title", node.get("name"))
        if isinstance(title, str) and title:
            titles.setdefault(str(key), title)
            inner = node.get("app_name", node.get("appName"))
            if isinstance(inner, str) and inner:
                titles.setdefault(inner, title)
    return titles


def game_titles(games: Path) -> dict[str, str]:
    """Map Heroic app names to display titles via Heroic's library caches.

    ``games`` is the ``GamesConfig`` directory; caches live alongside it in
    ``<heroic-root>/store_cache/``. Missing or corrupt caches are ignored so
    listing never fails; unknown games fall back to their app name.
    """
    heroic_root = games.parent if games.name == "GamesConfig" else games
    cache = heroic_root / "store_cache"
    titles: dict[str, str] = {}
    for filename in ("legendary_library.json", "gog_library.json", "nile_library.json"):
        try:
            doc = json.loads((cache / filename).read_text())
        except (OSError, json.JSONDecodeError):
            continue
        for app, title in _titles_from_library_doc(doc).items():
            titles.setdefault(app, title)
    for filename in ("legendary_install_info.json", "gog_install_info.json",
                     "nile_install_info.json"):
        try:
            doc = json.loads((cache / filename).read_text())
        except (OSError, json.JSONDecodeError):
            continue
        for app, title in _titles_from_install_info(doc).items():
            titles.setdefault(app, title)
    return titles


def list_games_with_titles(games: Path) -> list[tuple[str, str]]:
    """(app_name, title) pairs for each GamesConfig file, sorted by title.

    Title resolution prefers Heroic's library caches and falls back to the
    ``winePrefix`` basename (e.g. ``/games/Heroic/Prefixes/Alan Wake 2``) and
    finally to the app name itself, so the hash is always accompanied by
    something human-readable.
    """
    apps = list_games(games)
    titles = game_titles(games)
    pairs: list[tuple[str, str]] = []
    for app in apps:
        title = titles.get(app)
        if not title:
            try:
                data = json.loads((games / f"{app}.json").read_text())
            except (OSError, json.JSONDecodeError):
                data = {}
            section = data.get(app, {}) if isinstance(data, dict) else {}
            prefix = section.get("winePrefix") if isinstance(section, dict) else None
            if isinstance(prefix, str) and prefix.strip():
                candidate = prefix.rstrip("/").split("/")[-1].split("\\")[-1].strip()
                if candidate:
                    title = candidate
        pairs.append((app, title or app))
    pairs.sort(key=lambda item: item[1].casefold())
    return pairs


def check_game(game: str) -> str:
    if not game or "/" in game or "\\" in game or game in (".", ".."):
        raise SystemExit(
            "game must be the Heroic app name (the GamesConfig/<AppName>.json "
            "basename, e.g. as listed in ~/.config/heroic/GamesConfig/)"
        )
    return game


def heroic_wrapper(root: Path, profile: str, follow: str | None = None) -> tuple[str, str]:
    """Render the (exe, args) wrapper entry Heroic prepends to the game command.

    Heroic treats `exe` as a literal executable token and shell-splits only
    the argument list. So the path is emitted raw (no shell quotes, even when
    it holds spaces) and a spaced follow target is quoted to arrive as one
    argument.
    """
    profile_args = "" if profile == "default" else f" --profile {profile}"
    follow_args = ""
    if follow:
        target = follow if not any(char.isspace() for char in follow) else f'"{follow}"'
        follow_args = f" --follow-children {target}"
    return str(bridge_executable_path(root)), f"run{profile_args}{follow_args} --"


def _records_path(state: Path) -> Path:
    return state / "heroic-launch-options.json"


def _load_records(state: Path) -> dict[str, object]:
    path = _records_path(state)
    if not path.is_file():
        return {"schema": 1, "games": {}}
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"invalid Heroic launch-option state {path}: {exc}") from exc
    if data.get("schema") != 1 or not isinstance(data.get("games"), dict):
        raise SystemExit(f"invalid Heroic launch-option state {path}")
    return data


def _save_records(state: Path, records: dict[str, object]) -> None:
    state.mkdir(parents=True, exist_ok=True)
    path = _records_path(state)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(records, indent=2) + "\n")
    os.replace(temporary, path)


def _key(config: Path, game: str) -> str:
    return f"{config.parent}:{game}"


def _read_section(path: Path, game: str) -> tuple[dict[str, object], list[object], list[object], bool]:
    """Return (document, wrapperOptions, enviromentOptions, existed).

    Heroic nests per-game settings under the app name
    (``GameConfig.getSettings`` reads ``settings[appName]`` and merges over
    defaults), so both access paths go through ``data[game]``. A missing file
    or section means Heroic defaults.
    """
    if not path.is_file():
        return {}, [], [], False
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"invalid Heroic game config {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise SystemExit(f"invalid Heroic game config {path}")
    section = data.get(game, {})
    if not isinstance(section, dict):
        raise SystemExit(f"invalid {game!r} section in Heroic game config {path}")
    wrappers = section.get("wrapperOptions", [])
    if not isinstance(wrappers, list):
        raise SystemExit(f"invalid wrapperOptions in Heroic game config {path}")
    env_entries = section.get("enviromentOptions", [])
    if not isinstance(env_entries, list):
        raise SystemExit(f"invalid enviromentOptions in Heroic game config {path}")
    for entry in env_entries:
        if (not isinstance(entry, dict) or not isinstance(entry.get("key"), str)
                or not isinstance(entry.get("value"), str)):
            raise SystemExit(f"invalid enviromentOptions entry in Heroic game config {path}")
    return data, wrappers, env_entries, True


def _merge_env(existing: list[object]) -> tuple[list[object], dict[str, str | None]]:
    """Ensure MANAGED_ENV keys exist with managed values.

    Returns (new_entries, previous) where previous maps each managed key to
    its prior value (None when absent). Unrelated entries keep order;
    managed keys keep their existing position when present, otherwise append
    in MANAGED_ENV_ORDER.
    """
    previous: dict[str, str | None] = {}
    by_key: dict[str, int] = {}
    for index, entry in enumerate(existing):
        assert isinstance(entry, dict)
        key = entry.get("key")
        if isinstance(key, str) and key not in by_key:
            by_key[key] = index
    new: list[object] = [dict(entry) for entry in existing  # type: ignore[union-attr]
                         if isinstance(entry, dict)]
    for key in MANAGED_ENV_ORDER:
        wanted = MANAGED_ENV[key]
        if key in by_key:
            current = new[by_key[key]]
            assert isinstance(current, dict)
            previous[key] = current.get("value")  # type: ignore[assignment]
            current["value"] = wanted
        else:
            previous[key] = None
            by_key[key] = len(new)
            new.append({"key": key, "value": wanted})
    return new, previous


def _restore_env(current: list[object], previous: dict[str, object]) -> list[object]:
    """Drop our management, restoring prior values where untouched.

    A managed key is restored (to its previous value, or deleted when it was
    absent) only while it still holds our managed value — a user edit after
    configure is preserved instead of being clobbered.
    """
    restored: list[object] = []
    for entry in current:
        if not isinstance(entry, dict):
            continue
        key = entry.get("key")
        if isinstance(key, str) and key in MANAGED_ENV:
            if entry.get("value") != MANAGED_ENV[key]:
                restored.append(entry)
                continue
            prior = previous.get(key)
            if isinstance(prior, str):
                restored.append({"key": key, "value": prior})
            # prior None/missing: drop the entry entirely
            continue
        restored.append(entry)
    return restored


def _write_config(path: Path, game: str, data: dict[str, object],
                  wrappers: list[object], env_entries: list[object]) -> None:
    # The file is shared with all of Heroic's per-game settings and sibling
    # top-level keys (version, explicit): only data[game] is touched.
    section = data.get(game, {})
    if not isinstance(section, dict):
        raise SystemExit(f"invalid {game!r} section in Heroic game config {path}")
    if wrappers:
        section["wrapperOptions"] = wrappers
    else:
        section.pop("wrapperOptions", None)
    if env_entries:
        section["enviromentOptions"] = env_entries
    else:
        section.pop("enviromentOptions", None)
    data[game] = section
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent, text=True)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w") as stream:
            stream.write(json.dumps(data, indent=2) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        if path.is_file():
            shutil.copymode(path, temporary)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _without_ours(wrappers: list[object], exe: str) -> tuple[list[object], list[object]]:
    """Split entries into (ours by exe, rest), preserving order of the rest."""
    ours = [entry for entry in wrappers
            if isinstance(entry, dict) and entry.get("exe") == exe]
    rest = [entry for entry in wrappers
            if not (isinstance(entry, dict) and entry.get("exe") == exe)]
    return ours, rest


def configure(games: Path, game: str, exe: str, args: str, state: Path) -> str:
    # Ownership model: our executable name identifies our wrapper slot, and
    # MANAGED_ENV identifies our env keys. Only same-exe entries and managed
    # env keys are ever replaced; every other entry, setting, and top-level
    # key is preserved key-for-key. Re-running converges instead of
    # refusing, because unrelated Heroic UI edits must not wedge this command
    # the way a dedicated launch-options field would.
    path = games / f"{game}.json"
    data, wrappers, env_entries, existed = _read_section(path, game)
    wanted = {"exe": exe, "args": args}
    _, rest = _without_ours(wrappers, exe)
    new = [wanted] + rest
    new_env, env_previous_now = _merge_env(env_entries)
    records = _load_records(state)
    stored = records["games"]
    assert isinstance(stored, dict)
    previous = stored.get(_key(path, game))
    created = (not existed if not isinstance(previous, dict)
               else bool(previous.get("created")))
    if isinstance(previous, dict) and isinstance(previous.get("env_previous"), dict):
        env_previous = previous["env_previous"]
    else:
        env_previous = env_previous_now
    stored[_key(path, game)] = {
        "configured": wanted, "created": created, "env_previous": env_previous,
    }
    if new == wrappers and new_env == env_entries:
        _save_records(state, records)
        return "unchanged"
    backup = path.with_name(path.name + ".dlss-bridge-backup")
    if existed and not backup.exists():
        shutil.copy2(path, backup)
    _write_config(path, game, data, new, new_env)
    _save_records(state, records)
    return "configured"


def remove(games: Path, game: str, exe: str, state: Path) -> str:
    records = _load_records(state)
    stored = records["games"]
    assert isinstance(stored, dict)
    path = games / f"{game}.json"
    key = _key(path, game)
    entry = stored.get(key)
    if not isinstance(entry, dict):
        raise SystemExit(f"DLSS Bridge has no saved launch option for Heroic game {game}")
    data, wrappers, env_entries, existed = _read_section(path, game)
    ours, rest = _without_ours(wrappers, exe)
    previous_env = entry.get("env_previous")
    if not isinstance(previous_env, dict):
        previous_env = {}
    restored_env = _restore_env(env_entries, previous_env)
    if not existed and not ours:
        # Nothing of ours was ever written and the file is still absent.
        del stored[key]
        _save_records(state, records)
        return "removed"
    section = data.get(game, {})
    assert isinstance(section, dict)
    if (entry.get("created") and not rest and not restored_env
            and set(section) <= {"wrapperOptions", "enviromentOptions"}
            and set(data) == {game}):
        # We created this file and nothing else has been added since -- not to
        # the app section and not as sibling top-level keys (version,
        # explicit): delete it to restore the exact prior state.
        path.unlink(missing_ok=True)
    else:
        _write_config(path, game, data, rest, restored_env)
    del stored[key]
    _save_records(state, records)
    return "removed"
