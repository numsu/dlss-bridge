"""Install and validate user-scoped neural runtime components."""
from __future__ import annotations
import hashlib, json, os, shutil, struct, sys, tempfile, urllib.request, zipfile
from pathlib import Path

def state_root() -> Path:
    override = os.environ.get("DLSS_BRIDGE_STATE_DIR")
    if override: return Path(override).expanduser().resolve()
    if sys.platform == "win32":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    else:
        base = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local" / "state"))
    return base / "dlss-bridge"

def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""): digest.update(block)
    return digest.hexdigest()

def model_path() -> Path:
    return state_root() / "models" / "nvngx_dlssnr.dll"

def model_source_path() -> Path:
    return state_root() / "models" / "source.json"

def validate_model(source: Path) -> None:
    if not source.is_file(): raise SystemExit(f"model DLL does not exist: {source}")
    with source.open("rb") as stream:
        header = stream.read(4096)
    if len(header) < 64 or header[:2] != b"MZ":
        raise SystemExit("neural runtime is not a Windows PE DLL")
    pe_offset = struct.unpack_from("<I", header, 0x3C)[0]
    if pe_offset + 26 > len(header) or header[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise SystemExit("neural runtime has an invalid PE header")
    machine = struct.unpack_from("<H", header, pe_offset + 4)[0]
    optional_magic = struct.unpack_from("<H", header, pe_offset + 24)[0]
    if machine != 0x8664 or optional_magic != 0x20B:
        raise SystemExit("neural runtime must be a 64-bit x86 Windows DLL")

def import_model(source: Path) -> dict[str, str]:
    source = source.expanduser().resolve()
    if not source.is_file(): raise SystemExit(f"model DLL does not exist: {source}")
    if source.name.lower() != "nvngx_dlssnr.dll":
        raise SystemExit("expected a file named nvngx_dlssnr.dll")
    validate_model(source)
    target = model_path()
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix(".tmp")
    shutil.copy2(source, temporary)
    os.replace(temporary, target)
    model_source_path().unlink(missing_ok=True)
    return {"installed": str(target), "sha256": sha256(target)}

def acquire_model(manifest_path: Path, force: bool = False) -> dict[str, str]:
    """Download and verify the pinned upstream bundle, then install its runtime."""
    manifest_path = manifest_path.expanduser().resolve()
    try:
        manifest = json.loads(manifest_path.read_text())
        source = manifest["dlss_neural_runtime"]
        expected_archive = source["archive_sha256"].lower()
        expected_model = source["sha256"].lower()
        member = source["archive_member"]
        url = source["url"]
    except (OSError, KeyError, TypeError, json.JSONDecodeError) as exc:
        raise SystemExit(f"invalid runtime source manifest {manifest_path}: {exc}") from exc

    target = model_path()
    if target.is_file():
        validate_model(target)
        current_hash = sha256(target)
        if current_hash == expected_model:
            status = "current"
        else:
            managed = False
            metadata_path = model_source_path()
            if metadata_path.is_file():
                try:
                    metadata = json.loads(metadata_path.read_text())
                    managed = metadata.get("installed_sha256") == current_hash
                except (OSError, json.JSONDecodeError):
                    pass
            if not force and not managed:
                return {
                    "status": "preserved-user-runtime",
                    "installed": str(target),
                    "sha256": current_hash,
                }
            status = "updated"
    else:
        status = "installed"

    if status != "current":
        target.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="dlss-bridge-runtime-") as temporary:
            archive = Path(temporary) / "runtime.zip"
            request = urllib.request.Request(url, headers={"User-Agent": "dlss-bridge-installer/1"})
            try:
                with urllib.request.urlopen(request, timeout=60) as response, archive.open("wb") as output:
                    shutil.copyfileobj(response, output, length=1024 * 1024)
            except Exception as exc:
                raise SystemExit(f"could not download neural runtime from {url}: {exc}") from exc
            expected_size = int(source.get("archive_size", 0))
            if expected_size and archive.stat().st_size != expected_size:
                raise SystemExit(
                    f"neural runtime archive size mismatch: expected {expected_size}, "
                    f"received {archive.stat().st_size}"
                )
            actual_archive = sha256(archive)
            if actual_archive != expected_archive:
                raise SystemExit(
                    "neural runtime archive checksum mismatch: "
                    f"expected {expected_archive}, received {actual_archive}"
                )
            extracted = Path(temporary) / source.get("filename", "nvngx_dlssnr.dll")
            try:
                with zipfile.ZipFile(archive) as bundle, bundle.open(member) as incoming, extracted.open("wb") as output:
                    shutil.copyfileobj(incoming, output, length=1024 * 1024)
            except (KeyError, OSError, zipfile.BadZipFile) as exc:
                raise SystemExit(f"could not extract {member} from the runtime archive: {exc}") from exc
            validate_model(extracted)
            actual_model = sha256(extracted)
            if actual_model != expected_model:
                raise SystemExit(
                    "neural runtime checksum mismatch: "
                    f"expected {expected_model}, received {actual_model}"
                )
            temporary_target = target.with_suffix(".tmp")
            shutil.copy2(extracted, temporary_target)
            os.replace(temporary_target, target)

    metadata = {
        "schema": 1,
        "source": source.get("release_page", url),
        "download_url": url,
        "version": source.get("version", "unknown"),
        "license": source.get("license", "NVIDIA RTX SDK License"),
        "license_url": source.get("license_url", ""),
        "installed_sha256": expected_model,
        "archive_sha256": expected_archive,
    }
    metadata_path = model_source_path()
    metadata_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_metadata = metadata_path.with_suffix(".tmp")
    temporary_metadata.write_text(json.dumps(metadata, indent=2) + "\n")
    os.replace(temporary_metadata, metadata_path)
    return {"status": status, "installed": str(target), "sha256": expected_model,
            "source": metadata["source"], "license": metadata["license_url"]}

