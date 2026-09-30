"""Reject non-system DLL imports and record the actual build inputs/output."""
import hashlib
import json
import pathlib
import re
import subprocess

root = pathlib.Path.cwd()
pe = (root / "output/pe.txt").read_text()
imports = sorted(re.findall(r"DLL Name: (\S+)", pe))
system = {
    "advapi32.dll", "avrt.dll", "bcrypt.dll", "crypt32.dll", "dwmapi.dll",
    "dwrite.dll", "gdi32.dll", "imm32.dll", "kernel32.dll", "msvcrt.dll",
    "ncrypt.dll", "ntdll.dll", "ole32.dll", "oleaut32.dll", "rpcrt4.dll",
    "secur32.dll", "shcore.dll", "shell32.dll", "shlwapi.dll", "user32.dll",
    "userenv.dll", "usp10.dll", "uxtheme.dll", "version.dll", "ws2_32.dll",
}
unexpected = [name for name in imports if name.lower() not in system and not name.lower().startswith(("api-ms-win-", "ext-ms-win-"))]
if not imports or unexpected:
    raise SystemExit(f"Runtime dependency check failed: {unexpected or 'empty import table'}")
required = "mpv_client_api_version mpv_create mpv_initialize mpv_destroy mpv_terminate_destroy mpv_command mpv_command_string mpv_set_option mpv_set_option_string mpv_get_property mpv_get_property_string mpv_set_property mpv_set_property_string mpv_free mpv_observe_property mpv_wait_event mpv_error_string mpv_render_context_create mpv_render_context_set_update_callback mpv_render_context_update mpv_render_context_render mpv_render_context_free".split()
missing = [name for name in required if not re.search(r"\b" + name + r"\b", pe)]
if missing:
    raise SystemExit(f"Missing mpv exports: {missing}")
dll = root / "output/libmpv-2.dll"
with dll.open("rb") as stream:
    sha = hashlib.file_digest(stream, "sha256").hexdigest()
report = {
    "sources": json.loads((root / "recipe/sources.lock.json").read_text()),
    "recipeId": (root / "prefix/.recipe-id").read_text().strip(),
    "packages": (root / "output/msys2-packages.txt").read_text().splitlines(),
    "compiler": subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0],
    "optimization": "-Os; FFmpeg enable-small; LTO for mpv/FFmpeg/libplacebo; shader chain without LTO",
    "dll": {"bytes": dll.stat().st_size, "sha256": sha, "architecture": "x64", "imports": imports},
}
(root / "output/build-provenance.json").write_text(json.dumps(report, indent=2) + "\n")
