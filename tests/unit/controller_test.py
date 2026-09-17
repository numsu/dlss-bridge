import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import zipfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("controller", root / "controller" / "dlss_bridge.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
from session import configure_neural_runtime

cfg = module.load_toml(root / "profiles" / "default.toml")
resolved = module.resolved_runtime(cfg)
assert resolved["execution_mode"] == "auto"
assert resolved["compute_adapter"] == "auto"
assert resolved["output_transport"] == "native"
assert resolved["neural_queue_mode"] == "unified"
assert resolved["neural_pipeline_frames"] == 2
assert resolved["neural_placement"] == "before_sr"
assert resolved["neural_working_scale"] == "1"
assert resolved["neural_passes"] == 1
assert resolved["gpu_timestamps"] == 1

profile = module.load_toml(root / "profiles" / "secondary-gpu.toml")
resolved = module.resolved_runtime(module.merge(cfg, profile))
assert resolved["execution_mode"] == "secondary_gpu"
assert resolved["compute_adapter"] == "auto"
assert resolved["neural_queue_mode"] == "unified"

try:
    module.resolved_runtime(module.merge(cfg, {"bridge": {"sync": False}}))
except SystemExit:
    pass
else:
    raise AssertionError("controller accepted a removed configuration fallback")

try:
    module.resolved_runtime(module.merge(
        cfg, {"execution": {"compute_adapter": "uuid:GPU-example"}}))
except SystemExit:
    pass
else:
    raise AssertionError("controller accepted an unimplemented adapter selector")

with tempfile.TemporaryDirectory() as temporary:
    old_root = module.ROOT
    module.ROOT = Path(temporary)
    (module.ROOT / "profiles").mkdir()
    (module.ROOT / "profiles" / "matched.toml").write_text(
        'schema = 1\n[match]\nexecutable = "Game.exe"\ngraphics_api = "vulkan"\n'
        '[capture]\nadapter = "ngx_vulkan"\n[execution]\nmode = "secondary_gpu"\n'
    )
    assert module.matching_profiles(["launcher", "/games/Game.exe"])
    assert not module.matching_profiles(["launcher", "/games/Other.exe"])
    module.ROOT = old_root

for invalid_scale in (float("nan"), float("inf")):
    try:
        module.resolved_runtime(module.merge(cfg, {"feature": {"working_scale": invalid_scale}}))
    except SystemExit:
        pass
    else:
        raise AssertionError(f"controller accepted invalid scale {invalid_scale}")

with tempfile.TemporaryDirectory() as temporary:
    template = "[DlssNr]\nRunBeforeSR=true\nDeferredDLSS=false\nWorkingScale=1\nPasses=1\nToggleKey=auto\n"
    expected = {
        "before_sr": ("true", "false"),
        "after_sr": ("false", "false"),
        "deferred_dlss": ("true", "true"),
    }
    for placement, (before, deferred) in expected.items():
        ini = Path(temporary) / f"{placement}.ini"
        ini.write_text(template)
        configure_neural_runtime(ini, {
            "neural_placement": placement,
            "neural_working_scale": "0.75",
            "neural_passes": "2",
        })
        configured = ini.read_text()
        assert f"RunBeforeSR={before}" in configured
        assert f"DeferredDLSS={deferred}" in configured
        assert "WorkingScale=0.75" in configured
        assert "Passes=2" in configured
        assert "ToggleKey=0x87" in configured

# Runtime acquisition is concise for people and structured only on request.
original_acquire_model = module.acquire_model
module.acquire_model = lambda _manifest, _force: {
    "status": "installed",
    "installed": "/state/models/nvngx_dlssnr.dll",
    "sha256": "abc",
}
try:
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        module.cmd_acquire_runtime(type("Args", (), {"force": False, "json": False})())
    assert output.getvalue() == "Neural runtime downloaded and verified.\n"

    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        module.cmd_acquire_runtime(type("Args", (), {"force": False, "json": True})())
    assert json.loads(output.getvalue())["status"] == "installed"
finally:
    module.acquire_model = original_acquire_model


# Steam profile validation must execute before any external configuration access.
original_steam_running = module.steam_running
module.steam_running = lambda: False
try:
    args = type("Args", (), {"profile": "../unsafe", "appid": "275850"})()
    try:
        module.cmd_steam_configure(args)
    except SystemExit as exc:
        assert "bundled profile name" in str(exc)
    else:
        raise AssertionError("unsafe Steam profile name was accepted")
finally:
    module.steam_running = original_steam_running


components = module.component_inventory()
assert any(c["id"] == "ngx-vulkan" and c["state"] == "integrated" for c in components)
assert any(c["id"] == "ngx-d3d12" and c["state"] == "planned" for c in components)
for component in (c for c in components if c["state"] == "integrated" and c["kind"] != "platform"):
    assert component["linkage"] == "static"
    assert component["abi"] == 1


def pe_dll() -> bytearray:
    pe = bytearray(512)
    pe[:2] = b"MZ"
    pe[0x3C:0x40] = (0x80).to_bytes(4, "little")
    pe[0x80:0x84] = b"PE\0\0"
    pe[0x84:0x86] = (0x8664).to_bytes(2, "little")
    pe[0x98:0x9A] = (0x20B).to_bytes(2, "little")
    return pe


# A launch materializes only user-state session files. The game directory and
# Proton command remain untouched except for replacing the game process with
# the session launcher in the child command.
with tempfile.TemporaryDirectory() as temporary:
    sandbox = Path(temporary)
    os.environ["DLSS_BRIDGE_STATE_DIR"] = str(sandbox / "state")
    payload = sandbox / "payload"
    bridge = payload / "bridge"
    optiscaler = payload / "optiscaler"
    backend = optiscaler / "OptiScaler"
    game = sandbox / "game"
    for directory in (bridge, backend, game):
        directory.mkdir(parents=True, exist_ok=True)
    (bridge / "dlss-bridge-launcher.exe").write_bytes(b"launcher")
    (bridge / "dlss5-vk-hook.dll").write_bytes(b"hook")
    (optiscaler / "OptiScaler.dll").write_bytes(b"opti")
    (optiscaler / "OptiScaler.ini").write_text(
        "[DlssNr]\nEnabled=true\nRunBeforeSR=false\nDeferredDLSS=true\n"
        "WorkingScale=0.5\nPasses=3\nToggleKey=auto\n"
    )
    (backend / "backend.dll").write_bytes(b"backend")
    game_exe = game / "Game.exe"
    game_exe.write_bytes(b"game")
    existing_loader = game / "vulkan-1.dll"
    existing_loader.write_bytes(b"owned-by-the-game")
    model_source = sandbox / "nvngx_dlssnr.dll"
    model_source.write_bytes(pe_dll())
    module.import_model(model_source)
    before = {p.relative_to(game): p.read_bytes() for p in game.rglob("*") if p.is_file()}

    original = ["/proton/proton", "waitforexitandrun", str(game_exe), "--quality", "high"]
    launch = module.prepare_session(
        payload, original,
        "neural_placement=before_sr\nneural_working_scale=1\nneural_passes=1\n",
        {"BASE": "1"})
    assert original[2] == str(game_exe)
    assert launch.command[:2] == original[:2]
    assert launch.command[2].endswith("dlss-bridge-launcher.exe")
    assert launch.command[3] == "--hook"
    assert launch.command[5] == "--"
    assert launch.command[6].endswith("Game.exe")
    assert launch.command[-2:] == ["--quality", "high"]
    assert launch.environment["DLSS_BRIDGE_CONFIG"].endswith("dlss5-vk-bridge.cfg")
    assert "WINEDLLOVERRIDES" not in launch.environment
    session_ini = (launch.directory / "OptiScaler.ini").read_text()
    assert "RunBeforeSR=true" in session_ini
    assert "DeferredDLSS=false" in session_ini
    assert "WorkingScale=1" in session_ini
    assert "Passes=1" in session_ini
    assert "ToggleKey=0x87" in session_ini
    assert not (launch.directory / "nvngx.dll_dlssnr.dll").exists()
    bridge_cfg = (launch.directory / "dlss5-vk-bridge.cfg").read_text()
    assert "neural_placement" not in bridge_cfg
    assert "neural_working_scale" not in bridge_cfg
    assert "neural_passes" not in bridge_cfg
    assert {p.relative_to(game): p.read_bytes() for p in game.rglob("*") if p.is_file()} == before
    (launch.directory / "dlss5-vk-bridge.log").write_text("test log\n")
    identifier = launch.identifier
    module.close_session(launch)
    assert not launch.directory.exists()
    assert (sandbox / "state" / "logs" / identifier / "dlss5-vk-bridge.log").is_file()
    assert {p.relative_to(game): p.read_bytes() for p in game.rglob("*") if p.is_file()} == before
    del os.environ["DLSS_BRIDGE_STATE_DIR"]


# Setup-time acquisition verifies the archive and DLL before publishing it.
with tempfile.TemporaryDirectory() as temporary:
    sandbox = Path(temporary)
    os.environ["DLSS_BRIDGE_STATE_DIR"] = str(sandbox / "state")
    pe = pe_dll()
    archive = sandbox / "upstream.zip"
    member = "bin/runtime/dlssnr/nvngx_dlssnr.dll"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
        bundle.writestr(member, pe)
    digest = lambda data: hashlib.sha256(data).hexdigest()
    manifest = sandbox / "runtime-sources.json"
    manifest.write_text(json.dumps({
        "schema": 1,
        "dlss_neural_runtime": {
            "version": "test",
            "filename": "nvngx_dlssnr.dll",
            "release_page": "https://example.invalid/test",
            "url": archive.as_uri(),
            "archive_size": archive.stat().st_size,
            "archive_sha256": digest(archive.read_bytes()),
            "archive_member": member,
            "sha256": digest(pe),
            "license": "test license",
            "license_url": "https://example.invalid/license",
        },
    }))
    acquired = module.acquire_model(manifest)
    assert acquired["status"] == "installed"
    installed = sandbox / "state" / "models" / "nvngx_dlssnr.dll"
    assert installed.read_bytes() == pe
    assert module.acquire_model(manifest)["status"] == "current"
    del os.environ["DLSS_BRIDGE_STATE_DIR"]
