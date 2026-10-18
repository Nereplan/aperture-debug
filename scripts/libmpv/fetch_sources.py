"""Fetch and verify locked source archives in the script's owned build tree."""
import hashlib
import json
import pathlib
import subprocess
import sys
import tarfile

root = pathlib.Path.cwd()
lock = json.loads((root / "recipe/sources.lock.json").read_text())
downloads = root / "downloads"
downloads.mkdir(exist_ok=True)
for entry in lock["sources"]:
    archive = downloads / (entry["name"] + ".tgz")
    if not archive.exists():
        partial = archive.with_suffix(".partial")
        subprocess.run(["curl", "--fail", "--location", "--retry", "3", "--output", str(partial), entry["url"]], check=True)
        partial.rename(archive)
    with archive.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != entry["sha256"]:
            raise SystemExit(f"Source checksum mismatch: {archive}")
    destination = root / entry["destination"]
    marker = destination / ".aperture-source-sha256"
    if marker.exists() and marker.read_text().strip() == entry["sha256"]:
        continue
    if destination.exists() and any(destination.iterdir()):
        raise SystemExit(f"Unverified source tree exists: {destination}; use a fresh BuildRoot")
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as tar:
        members = tar.getmembers()
        top = members[0].name.split("/")[0] + "/"
        for member in members:
            if not member.name.startswith(top):
                continue
            member.name = member.name[len(top):]
            if member.name:
                tar.extract(member, destination, filter="data")
    marker.write_text(entry["sha256"] + "\n")
    print(f"Verified source: {entry['name']} {entry['revision']}", flush=True)

subprocess.run([sys.executable, "recipe/apply_source_patches.py"], check=True)
