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
