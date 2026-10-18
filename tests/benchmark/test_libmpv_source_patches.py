"""Behavioral checks for the local library build's pinned source patches."""
import difflib
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("source_patches", REPO / "scripts/libmpv/apply_source_patches.py")
PATCHES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCHES)
PATCH_COMMAND = shutil.which("patch") or ("C:/msys64/usr/bin/patch.exe" if Path("C:/msys64/usr/bin/patch.exe").exists() else None)


@unittest.skipUnless(PATCH_COMMAND, "GNU patch is required for the native library build")
class SourcePatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="aperture-source-patches-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "src/ffmpeg"
        self.recipe = self.root / "recipe"
        self.source.mkdir(parents=True)
        self.recipe.mkdir()
        self.before = {"a.c": "int a = 1;\n", "b.c": "int b = 2;\n"}
        self.after = {"a.c": "int a = 10;\n", "b.c": "int b = 20;\n"}
        payload = "".join("".join(difflib.unified_diff(self.before[name].splitlines(True), self.after[name].splitlines(True),
                            fromfile="a/"+name, tofile="b/"+name)) for name in self.before)
        (self.recipe / "test.patch").write_bytes(payload.encode())
        self.manifest = {"patches": [{"id": "test", "sourceRoot": "src/ffmpeg", "patch": "test.patch",
            "patchSha256": hashlib.sha256(payload.encode()).hexdigest(), "files": [
                {"path": name, "beforeSha256": hashlib.sha256(self.before[name].encode()).hexdigest(),
                 "afterSha256": hashlib.sha256(self.after[name].encode()).hexdigest()} for name in self.before]}]}
        self.save_manifest()
        for name, text in self.before.items():
            (self.source / name).write_bytes(text.encode())

    def save_manifest(self):
        (self.recipe / "source_patches.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def apply(self, **kwargs):
        return PATCHES.apply_patches(self.root, patch_command=PATCH_COMMAND, **kwargs)

    def assert_sources(self, expected):
        self.assertEqual({name: (self.source / name).read_text() for name in expected}, expected)

    def test_applies_complete_patch_and_repeated_calls_preserve_it(self):
        self.apply()
        self.assert_sources(self.after)
        self.apply()
        self.assert_sources(self.after)

    def test_rejects_edited_source_before_touching_other_files(self):
        (self.source / "a.c").write_text("user edit\n")
        with self.assertRaises(ValueError):
            self.apply()
        self.assert_sources({"a.c": "user edit\n", "b.c": self.before["b.c"]})

    def test_rejects_partially_applied_patch(self):
        (self.source / "a.c").write_text(self.after["a.c"])
        with self.assertRaises(ValueError):
            self.apply()
        self.assert_sources({"a.c": self.after["a.c"], "b.c": self.before["b.c"]})

    def test_rejects_changed_patch_payload(self):
        with (self.recipe / "test.patch").open("a") as stream:
            stream.write("unexpected patch edit\n")
        with self.assertRaises(ValueError):
            self.apply()
        self.assert_sources(self.before)

    def test_check_only_requires_already_patched_sources(self):
        with self.assertRaises(ValueError):
            self.apply(check_only=True)
        self.assert_sources(self.before)
        self.apply()
        self.apply(check_only=True)

    def test_rejects_path_outside_source_tree(self):
        self.manifest["patches"][0]["files"][0]["path"] = "../a.c"
        self.save_manifest()
        with self.assertRaises(ValueError):
            self.apply()
        self.assert_sources(self.before)

    def test_bad_second_hunk_leaves_first_file_unchanged(self):
        path = self.recipe / "test.patch"
        payload = path.read_bytes().replace(b"-int b = 2;", b"-unmatched context;")
        path.write_bytes(payload)
        self.manifest["patches"][0]["patchSha256"] = hashlib.sha256(payload).hexdigest()
        self.save_manifest()
        with self.assertRaises(ValueError):
            self.apply()
        self.assert_sources(self.before)


if __name__ == "__main__":
    unittest.main()
