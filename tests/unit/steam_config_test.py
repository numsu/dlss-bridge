import json
from pathlib import Path
import tempfile

import sys
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "controller"))
import steam_config

sample = '''"UserLocalConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n\t\t\t\t"apps"\n\t\t\t\t{\n\t\t\t\t\t"275850"\n\t\t\t\t\t{\n\t\t\t\t\t\t"LastPlayed"\t\t"1"\n\t\t\t\t\t}\n\t\t\t\t\t"730"\n\t\t\t\t\t{\n\t\t\t\t\t\t"LaunchOptions"\t\t"gamemoderun %command%"\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n'''
option = "/home/gamer/.local/bin/dlss-bridge run --profile same-gpu -- %command%"
updated = steam_config.set_launch_option(sample, "275850", option)
assert steam_config.read_launch_option(updated, "275850") == option
assert steam_config.read_launch_option(updated, "730") == "gamemoderun %command%"
assert steam_config.set_launch_option(updated, "275850", None) == sample

replaced = steam_config.set_launch_option(sample, "730", option)
assert steam_config.read_launch_option(replaced, "730") == option
assert steam_config.set_launch_option(replaced, "730", None).count('"LaunchOptions"') == 0

with tempfile.TemporaryDirectory() as temporary:
    directory = Path(temporary)
    config = directory / "localconfig.vdf"
    state = directory / "state"
    config.write_text(sample)
    assert steam_config.configure(config, "730", option, state) == "configured"
    assert steam_config.read_launch_option(config.read_text(), "730") == option
    record = json.loads((state / "steam-launch-options.json").read_text())
    assert next(iter(record["games"].values()))["previous"] == "gamemoderun %command%"
    assert steam_config.remove(config, "730", state) == "removed"
    assert config.read_text() == sample
    assert config.with_name("localconfig.vdf.dlss-bridge-backup").read_text() == sample


with tempfile.TemporaryDirectory() as temporary:
    directory = Path(temporary)
    config = directory / "40558437" / "config" / "localconfig.vdf"
    config.parent.mkdir(parents=True)
    state = directory / "state"
    config.write_text(sample)
    steam_config.configure(config, "730", option, state)
    config.write_text(steam_config.set_launch_option(config.read_text(), "730", "user edit"))
    try:
        steam_config.configure(config, "730", option + " changed", state)
    except SystemExit as exc:
        assert "refusing to overwrite" in str(exc)
    else:
        raise AssertionError("a later user change was overwritten")

try:
    steam_config.read_launch_option(sample, "999999")
except KeyError:
    pass
else:
    raise AssertionError("missing AppID was accepted")

# bridge_command renders GPU profiles and the follow-children target into a
# Steam launch option; spaced names are quoted so Steam parses one argument.
rendered = steam_config.bridge_command(Path("/app"), "default", None)
assert rendered.endswith("run -- %command%")
rendered = steam_config.bridge_command(Path("/app"), "same-gpu", "Game.exe")
assert rendered.endswith("run --profile same-gpu --follow-children Game.exe -- %command%")
rendered = steam_config.bridge_command(Path("/app"), "default", "My Game.exe")
assert '--follow-children "My Game.exe"' in rendered

# Library discovery reads both libraryfolders.vdf schema generations, and
# installed_games pairs AppIDs with names while skipping corrupt manifests.
with tempfile.TemporaryDirectory() as temporary:
    sandbox = Path(temporary)
    first = sandbox / "lib"
    second = sandbox / "extra"
    (first / "steamapps").mkdir(parents=True)
    (second / "steamapps").mkdir(parents=True)
    third = sandbox / "third"
    (third / "steamapps").mkdir(parents=True)
    (third / "steamapps" / "appmanifest_50.acf").write_text(
        '"AppState"\n{\n\t"appid"\t\t"50"\n\t"name"\t\t"Middle Game"\n}\n')
    (first / "steamapps" / "libraryfolders.vdf").write_text(
        '"libraryfolders"\n{\n'
        '"TimeNextStatsReport"\t\t"1320000000"\n'
        '"1"\t\t"' + str(second).replace("\\", "\\\\") + '"\n'
        '"2"\n{\n\t\t"path"\t\t"' + str(third).replace("\\", "\\\\") + '"\n}\n'
        '}\n')
    (first / "steamapps" / "appmanifest_20.lcf").write_text("")
    (first / "steamapps" / "appmanifest_20.acf").write_text(
        '"AppState"\n{\n\t"appid"\t\t"20"\n\t"name"\t\t"Zebra Game"\n}\n')
    (second / "steamapps" / "appmanifest_10.acf").write_text(
        '"AppState"\n{\n\t"appid"\t\t"10"\n\t"name"\t\t"Alpha Game"\n'
        '\t"Universe"\t\t"1"\n}\n')
    (second / "steamapps" / "appmanifest_20.acf").write_text(
        '"AppState"\n{\n\t"appid"\t\t"20"\n\t"name"\t\t"Zebra Duplicate"\n}\n')
    (second / "steamapps" / "appmanifest_30.acf").write_text(
        '"AppState"\n{\n\t"appid"\t\t"30"\n}\n')
    (second / "steamapps" / "appmanifest_40.acf").write_text("{broken")
    libraries = steam_config.library_folders(first)
    assert libraries[0] == first.resolve()
    assert second.resolve() in libraries
    assert third.resolve() in libraries
    assert steam_config.installed_games(libraries) == [
        ("10", "Alpha Game"), ("50", "Middle Game"), ("20", "Zebra Game")]
    assert steam_config.library_folders(sandbox / "missing") == []
    assert steam_config.installed_games([]) == []
