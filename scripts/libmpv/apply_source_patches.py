"""Apply pinned local patches only to their exact locked source versions."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def owned_path(root, relative):
    parts = PurePosixPath(relative)
    if not parts.parts or parts.is_absolute() or any(p in ("..", ".") or ":" in p or "\\" in p for p in parts.parts):
        raise ValueError(f"Invalid patch path: {relative}")
    target = root.joinpath(*parts.parts)
    for node in (target, *target.parents):
        if node == root:
            break
        if node.is_symlink() or (hasattr(node, "is_junction") and node.is_junction()):
            raise ValueError(f"Reparse point in patch path: {relative}")
    if not target.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"Patch path escapes build root: {relative}")
    return target


def apply_patches(root, *, check_only=False, patch_command="patch"):
    root = Path(root).resolve()
    recipe = root / "recipe"
    manifest = json.loads((recipe / "source_patches.json").read_text(encoding="utf-8"))
    for entry in manifest["patches"]:
        patch = owned_path(recipe, entry["patch"])
        if digest(patch) != entry["patchSha256"]:
            raise ValueError(f"Patch checksum mismatch: {entry['id']}")
        source = owned_path(root, entry["sourceRoot"])
        files = [(owned_path(source, row["path"]), row) for row in entry["files"]]
        hashes = [digest(path) for path, _ in files]
        if all(actual == row["afterSha256"] for actual, (_, row) in zip(hashes, files)):
            continue
        if check_only or not all(actual == row["beforeSha256"] for actual, (_, row) in zip(hashes, files)):
            raise ValueError(f"Unrecognized or partially patched sources: {entry['id']}")
        # Validate the complete patch before touching any source file.
        for dry_run in (True, False):
            command = [patch_command, "--batch", "--forward", "-p1"]
            if dry_run:
                command.append("--dry-run")
            result = subprocess.run(command, input=patch.read_bytes(), cwd=source, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if result.returncode:
                raise ValueError(f"Source patch failed: {entry['id']}\n{result.stdout.decode('utf-8', errors='replace')[-3000:]}")
        if any(digest(path) != row["afterSha256"] for path, row in files):
            raise ValueError(f"Patched source checksum mismatch: {entry['id']}")
        print(f"Applied source patch: {entry['id']}", flush=True)
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    apply_patches(Path.cwd(), check_only=args.check_only)
