"""Exercise the actual native recipe profile selection, without building it."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
MSYS_BASH = Path("C:/msys64/usr/bin/bash.exe")
BASH = str(MSYS_BASH) if MSYS_BASH.exists() else shutil.which("bash")


@unittest.skipUnless(BASH, "Bash is required for the native recipe")
class NativeBuildProfiles(unittest.TestCase):
    def select(self, profile=None):
        env = dict(os.environ)
        env.pop("APERTURE_NATIVE_PROFILE", None)
        if profile is not None:
            env["APERTURE_NATIVE_PROFILE"] = profile
        script = "set -e; source " + shlex.quote((ROOT / "scripts/libmpv/build_profile.sh").as_posix())
        script += '; printf "%s\\n" "$NATIVE_PROFILE" "$FFMPEG_SMALL_FLAG" "$NATIVE_OPT_FLAG" "$MESON_OPT_LEVEL"'
        return subprocess.run([BASH, "--noprofile", "--norc", "-c", script],
                              env=env, text=True, capture_output=True, timeout=10)

    def test_default_preserves_size_profile(self):
        result = self.select()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["size", "--enable-small", "-Os", "s"])

    def test_explicit_size_matches_default(self):
        self.assertEqual(self.select("size").stdout, self.select().stdout)

    def test_performance_changes_optimization_without_fast_math(self):
        result = self.select("performance")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["performance", "--disable-small", "-O3", "3"])

    def test_balanced_keeps_fast_decoder_paths_with_o2(self):
        result = self.select("balanced")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["balanced", "--disable-small", "-O2", "2"])

    def test_unknown_profile_stops_before_any_build(self):
        result = self.select("unknown")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertIn("Unknown native optimization profile", result.stderr)


if __name__ == "__main__":
    unittest.main()
