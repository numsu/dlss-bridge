import json
from pathlib import Path
import tempfile

import sys
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "controller"))
import heroic_config

EXE = "/home/gamer/.local/bin/dlss-bridge"
ARGS = "run --profile same-gpu --"


def games_under(temporary):
    games = Path(temporary) / "heroic" / "GamesConfig"
    games.mkdir(parents=True)
    return games


def heroic_reads_wrappers(path, game):
    """Mirror of Heroic's own reader (GameConfig.getSettings): settings live
    nested under the app name and merge over defaults. Every file this tool
    writes must satisfy this function, or Heroic ignores the wrapper."""
    data = json.loads(Path(path).read_text())
    section = data.get(game, {})
    assert isinstance(section, dict), "Heroic settings must nest under the app name"
    wrappers = section.get("wrapperOptions", [])
    assert isinstance(wrappers, list)
    return wrappers


# Configuring a game without a settings file creates a minimal one holding
# only our wrapper; Heroic merges the rest from its defaults at runtime.
with tempfile.TemporaryDirectory() as temporary:
    sandbox = Path(temporary)
    games = games_under(sandbox)
    state = sandbox / "state"
    assert heroic_config.configure(games, "SomeGame", EXE, ARGS, state) == "configured"
    config = games / "SomeGame.json"
    assert heroic_reads_wrappers(config, "SomeGame") == [{"exe": EXE, "args": ARGS}]
    assert heroic_config.configure(games, "SomeGame", EXE, ARGS, state) == "unchanged"
    assert heroic_config.remove(games, "SomeGame", EXE, state) == "removed"
    assert not config.exists()
    try:
        heroic_config.remove(games, "SomeGame", EXE, state)
    except SystemExit as exc:
        assert "no saved launch option" in str(exc)
    else:
        raise AssertionError("second remove was accepted")


# Unrelated wrappers, settings, and top-level keys survive configure and
# remove untouched, and our entry always lands first.
with tempfile.TemporaryDirectory() as temporary:
    sandbox = Path(temporary)
    games = games_under(sandbox)
    state = sandbox / "state"
    config = games / "OtherGame.json"
    config.write_text(json.dumps({
        "OtherGame": {
            "wineVersion": {"name": "GE-Proton"},
            "wrapperOptions": [{"exe": "gamemoderun", "args": ""}],
        },
        "version": "v0",
    }))
    assert heroic_config.configure(games, "OtherGame", EXE, ARGS, state) == "configured"
    wrappers = heroic_reads_wrappers(config, "OtherGame")
    assert wrappers[0] == {"exe": EXE, "args": ARGS}
    assert wrappers[1] == {"exe": "gamemoderun", "args": ""}
    data = json.loads(config.read_text())
    assert data["OtherGame"]["wineVersion"] == {"name": "GE-Proton"}
    assert data["version"] == "v0"
    assert heroic_config.remove(games, "OtherGame", EXE, state) == "removed"
    assert heroic_reads_wrappers(config, "OtherGame") == [{"exe": "gamemoderun", "args": ""}]
    data = json.loads(config.read_text())
    assert data["OtherGame"]["wineVersion"] == {"name": "GE-Proton"}
    assert data["version"] == "v0"


# A file this tool created may later grow sibling top-level keys (Heroic adds
# version/explicit itself). Removing the wrapper must then preserve the file
# and drop only our entry, never delete the document.
with tempfile.TemporaryDirectory() as temporary:
    sandbox = Path(temporary)
    games = games_under(sandbox)
    state = sandbox / "state"
    config = games / "LateKeys.json"
    assert heroic_config.configure(games, "LateKeys", EXE, ARGS, state) == "configured"
    data = json.loads(config.read_text())
    data["explicit"] = "steam"
    data["version"] = 1
    config.write_text(json.dumps(data))
    assert heroic_config.remove(games, "LateKeys", EXE, state) == "removed"
    assert config.exists()
    data = json.loads(config.read_text())
    assert data["explicit"] == "steam"
    assert data["version"] == 1
    assert "wrapperOptions" not in data["LateKeys"]


# Reconfiguring with different args replaces our slot instead of stacking.
with tempfile.TemporaryDirectory() as temporary:
    sandbox = Path(temporary)
    games = games_under(sandbox)
    state = sandbox / "state"
    heroic_config.configure(games, "SwapGame", EXE, ARGS, state)
    assert heroic_config.configure(games, "SwapGame", EXE, "run --", state) == "configured"
    assert heroic_reads_wrappers(games / "SwapGame.json", "SwapGame") == [
        {"exe": EXE, "args": "run --"}]
    assert heroic_config.remove(games, "SwapGame", EXE, state) == "removed"


# Corrupt configs and bad game names fail loudly instead of writing.
with tempfile.TemporaryDirectory() as temporary:
    sandbox = Path(temporary)
    games = games_under(sandbox)
    state = sandbox / "state"
    broken = games / "Broken.json"
    broken.write_text("{nope")
    try:
        heroic_config.configure(games, "Broken", EXE, ARGS, state)
    except SystemExit as exc:
        assert "invalid Heroic game config" in str(exc)
    else:
        raise AssertionError("corrupt Heroic config was accepted")
    heroic_config.configure(games, "Fixed", EXE, ARGS, state)
    (games / "Fixed.json").write_text("{nope")
    try:
        heroic_config.remove(games, "Fixed", EXE, state)
    except SystemExit as exc:
        assert "invalid Heroic game config" in str(exc)
    else:
        raise AssertionError("corrupt Heroic config was accepted")
    for bad in ("", "dir/game", "..", "a/b"):
        try:
            heroic_config.check_game(bad)
        except SystemExit:
            pass
        else:
            raise AssertionError(f"bad Heroic game name was accepted: {bad!r}")


# Root selection prefers explicit roots and complains about ambiguity.
import os
with tempfile.TemporaryDirectory() as temporary:
    sandbox = Path(temporary)
    first = sandbox / "one" / "heroic"
    second = sandbox / "two" / "heroic"
    (first / "GamesConfig").mkdir(parents=True)
    (second / "GamesConfig").mkdir(parents=True)
    old_home, old_xdg = os.environ.get("HOME"), os.environ.get("XDG_CONFIG_HOME")
    os.environ["HOME"] = str(sandbox / "home")
    os.environ["XDG_CONFIG_HOME"] = str(sandbox / "xdg")
    try:
        (sandbox / "home" / ".config" / "heroic" / "GamesConfig").mkdir(parents=True)
        flat = (sandbox / "home" / ".var" / "app" / "com.heroicgameslauncher.hgl"
                / "config" / "heroic" / "GamesConfig")
        flat.mkdir(parents=True)
        try:
            heroic_config.select_games_config(None)
        except SystemExit as exc:
            assert "--heroic-root" in str(exc)
        else:
            raise AssertionError("ambiguous Heroic roots were accepted")
    finally:
        if old_home is None:
            del os.environ["HOME"]
        else:
            os.environ["HOME"] = old_home
        if old_xdg is None:
            os.environ.pop("XDG_CONFIG_HOME", None)
        else:
            os.environ["XDG_CONFIG_HOME"] = old_xdg
    assert heroic_config.select_games_config(first) == first / "GamesConfig"
    try:
        heroic_config.select_games_config(sandbox / "missing")
    except SystemExit as exc:
        assert "no Heroic configuration" in str(exc)
    else:
        raise AssertionError("missing Heroic root was accepted")


# Listing reports per-game settings basenames and skips non-JSON files.
with tempfile.TemporaryDirectory() as temporary:
    sandbox = Path(temporary)
    games = games_under(sandbox)
    (games / "B.json").write_text("{}")
    (games / "A.json").write_text("{}")
    (games / "notes.txt").write_text("x")
    assert heroic_config.list_games(games) == ["A", "B"]
    assert heroic_config.list_games(sandbox / "missing") == []


# Titled listing resolves library-cache titles, falls back to the winePrefix
# basename and finally to the app name, and sorts by title.
with tempfile.TemporaryDirectory() as temporary:
    sandbox = Path(temporary)
    games = games_under(sandbox)
    heroic_root = games.parent
    (games / "b-hash.json").write_text(json.dumps(
        {"b-hash": {"winePrefix": "/games/Heroic/Prefixes/Zebra Game"}}))
    (games / "a-hash.json").write_text(json.dumps(
        {"a-hash": {"winePrefix": "/games/Heroic/Prefixes/Alpha Game"}}))
    (games / "plain.json").write_text(json.dumps({}))
    cache = heroic_root / "store_cache"
    cache.mkdir()
    (cache / "legendary_library.json").write_text(json.dumps(
        {"library": [{"app_name": "b-hash", "title": "Library Title"}]}))
    assert heroic_config.list_games_with_titles(games) == [
        ("a-hash", "Alpha Game"), ("b-hash", "Library Title"), ("plain", "plain")]
    (cache / "legendary_library.json").write_text("{broken")
    assert heroic_config.list_games_with_titles(games) == [
        ("a-hash", "Alpha Game"), ("plain", "plain"), ("b-hash", "Zebra Game")]


# Wrapper rendering splits exe from args for Heroic's {exe, args} entries.
with tempfile.TemporaryDirectory() as temporary:
    sandbox = Path(temporary)
    payload = sandbox / "libexec" / "dlss-bridge"
    payload.mkdir(parents=True)
    command = sandbox / "bin" / "dlss-bridge"
    command.parent.mkdir(parents=True)
    command.write_text("#!/bin/sh\n")
    exe, args = heroic_config.heroic_wrapper(payload, "default", None)
    assert exe == str(command)
    assert args == "run --"
    exe, args = heroic_config.heroic_wrapper(payload, "same-gpu", "Game.exe")
    assert args == "run --profile same-gpu --follow-children Game.exe --"

# A spaced install path must reach Heroic raw: it is a literal executable
# token (Heroic shell-splits only the args), so shell quoting would become
# part of the filename.
with tempfile.TemporaryDirectory() as temporary:
    rootdir = Path(temporary) / "my app"
    command = rootdir / "bin" / "dlss-bridge"
    command.parent.mkdir(parents=True)
    command.write_text("#!/bin/sh\n")
    payload = rootdir / "libexec" / "dlss-bridge"
    payload.mkdir(parents=True)
    exe, _ = heroic_config.heroic_wrapper(payload, "default", None)
    assert exe == str(command)
    assert '"' not in exe
