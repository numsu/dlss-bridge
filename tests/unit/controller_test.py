import importlib.util
from pathlib import Path
import tempfile

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("controller", root / "controller" / "dlss_bridge.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

cfg = module.load_toml(root / "profiles" / "default.toml")
resolved = module.resolved_runtime(cfg)
assert resolved["require_neural_result"] == 1
assert resolved["execution_mode"] == "auto"
assert resolved["compute_adapter"] == "auto"
assert resolved["output_transport"] == "native"
assert resolved["neural_queue_mode"] == "split"
assert resolved["neural_placement"] == "before_sr"
assert resolved["neural_working_scale"] == "1"
assert resolved["neural_passes"] == 1
assert resolved["neural_runtime_variant"] == "presr-v0.7.7"

profile = module.load_toml(root / "profiles" / "no-mans-sky-vulkan.toml")
merged = module.merge(cfg, profile)
resolved = module.resolved_runtime(merged)
assert resolved["execution_mode"] == "secondary_gpu"
assert resolved["compute_adapter"] == "index:1"
assert resolved["output_transport"] == "native"
assert resolved["neural_queue_mode"] == "split"
assert resolved["neural_placement"] == "before_sr"
assert resolved["neural_working_scale"] == "1"
assert resolved["neural_runtime_variant"] == "presr-v0.7.7"

for invalid_scale in (float("nan"), float("inf")):
    invalid = module.merge(cfg, {"feature": {"working_scale": invalid_scale}})
    try:
        module.resolved_runtime(invalid)
    except SystemExit:
        pass
    else:
        raise AssertionError(f"controller accepted invalid scale {invalid_scale}")

try:
    module.inside_root(Path("/tmp/outside-dlss-project"))
except SystemExit:
    pass
else:
    raise AssertionError("controller allowed a write outside the project")

components = module.component_inventory()
assert any(c["id"] == "ngx-vulkan" and c["state"] == "integrated" for c in components)
assert any(c["id"] == "ngx-d3d12" and c["state"] == "planned" for c in components)
assert any(c["id"] == "native-linux" for c in components)
