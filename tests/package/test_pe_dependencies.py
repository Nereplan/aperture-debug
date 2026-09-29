#!/usr/bin/env python3
"""
Unit and integration tests for PE dependency inspection and packaging rules.

Tests verify:
1. Pure Python PE parser correctness on x64 executables and DLLs.
2. Robust rejection of invalid files, corrupt headers, and non-PE data.
3. Architecture constraints (strict x64 machine 0x8664 requirement).
4. Static runtime linkage constraints (no MSVCP140 / VCRUNTIME140 imports).
5. Published release binary closure for Aperture.exe and libmpv-2.dll.
"""

import os
import struct
import sys
import unittest

# Ensure the package test directory is in Python path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from inspect_pe_imports import (
    FORBIDDEN_DYNAMIC_RUNTIMES,
    PEInspectionError,
    PEInspector,
    inspect_files,
)
from compare_pe_reproducibility import (
    PEDetailedInspector,
    ReproducibilityComparator,
)

class TestPEInspector(unittest.TestCase):
    """Test suite for PE header and dependency parsing."""

    @classmethod
    def setUpClass(cls):
        cls.workspace_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
        cls.exe_path = os.path.join(cls.workspace_root, "release", "Aperture.exe")
        cls.mpv_path = os.path.join(cls.workspace_root, "release", "libmpv-2.dll")
        cls.verify_exe_path = os.path.join(cls.workspace_root, "Aperture.verify.exe")

    def test_missing_file_raises(self):
        """Non-existent file must raise PEInspectionError."""
        with self.assertRaises(PEInspectionError):
            PEInspector("non_existent_binary_file_12345.exe").parse()

    def test_invalid_magic_raises(self):
        """Files without MZ header must raise PEInspectionError."""
        dummy_path = os.path.join(os.path.dirname(__file__), "test_dummy_text.txt")
        try:
            with open(dummy_path, "wb") as f:
                f.write(b"Hello world, this is not an executable.")
            with self.assertRaises(PEInspectionError):
                PEInspector(dummy_path).parse()
        finally:
            if os.path.exists(dummy_path):
                os.remove(dummy_path)

    def test_corrupt_pe_header_raises(self):
        """Files with MZ header but corrupt PE offset must raise PEInspectionError."""
        dummy_path = os.path.join(os.path.dirname(__file__), "test_dummy_corrupt.exe")
        try:
            # Construct a minimal buffer with MZ signature and invalid e_lfanew
            buf = bytearray(128)
            buf[0:2] = b"MZ"
            struct.pack_into("<I", buf, 0x3C, 0x1000)  # offset exceeds length
            with open(dummy_path, "wb") as f:
                f.write(buf)
            with self.assertRaises(PEInspectionError):
                PEInspector(dummy_path).parse()
        finally:
            if os.path.exists(dummy_path):
                os.remove(dummy_path)

    def test_aperture_exe_x64_and_static_runtime(self):
        """Aperture.exe must be x64 and statically link the C/C++ runtime."""
        target = self.verify_exe_path if os.path.isfile(self.verify_exe_path) else self.exe_path
        if not os.path.isfile(target):
            self.skipTest("No Aperture executable found in workspace.")

        report = PEInspector(target).parse()
        self.assertEqual(report["machine"], "0x8664", f"{target} must be x64 machine 0x8664")
        self.assertTrue(report["is_64bit"], f"{target} must have 64-bit optional header")
        self.assertEqual(report["machineName"], "x64")

        # Verify static runtime constraint
        upper_imports = [imp.upper() for imp in report["imports"]]
        for forbidden in FORBIDDEN_DYNAMIC_RUNTIMES:
            self.assertNotIn(
                forbidden,
                upper_imports,
                f"Release binary {target} must not import dynamic runtime {forbidden}"
            )

        # Verify expected core Win32 / Direct2D dependencies
        self.assertTrue(
            any("D2D1" in imp.upper() for imp in report["imports"]),
            f"{target} must import Direct2D (d2d1.dll)"
        )
        self.assertTrue(
            any("KERNEL32" in imp.upper() for imp in report["imports"]),
            f"{target} must import KERNEL32.dll"
        )
        self.assertTrue(
            any("USER32" in imp.upper() for imp in report["imports"]),
            f"{target} must import USER32.dll"
        )

    def test_libmpv_x64_dependencies(self):
        """libmpv-2.dll must be x64 and have valid import table."""
        if not os.path.isfile(self.mpv_path):
            self.skipTest("libmpv-2.dll not found in workspace.")

        report = PEInspector(self.mpv_path).parse()
        self.assertEqual(report["machine"], "0x8664", "libmpv-2.dll must be x64 machine 0x8664")
        self.assertTrue(report["is_64bit"], "libmpv-2.dll must have 64-bit optional header")
        self.assertGreater(len(report["imports"]), 0, "libmpv-2.dll must have imported DLLs")

    def test_inspect_files_aggregate_report(self):
        """inspect_files helper returns aggregate report dictionary."""
        files = []
        if os.path.isfile(self.exe_path):
            files.append(self.exe_path)
        if os.path.isfile(self.mpv_path):
            files.append(self.mpv_path)
        if not files:
            self.skipTest("No workspace binaries found.")

        result = inspect_files(files)
        self.assertIn("reports", result)
        self.assertEqual(len(result["reports"]), len(files))
        for r in result["reports"]:
            self.assertEqual(r["machine"], "0x8664")


    def test_dependency_manifest_integrity(self):
        """scripts/dependencies.json must exist and accurately record vendored artifact metadata."""
        import hashlib
        import json
        manifest_path = os.path.join(self.workspace_root, "scripts", "dependencies.json")
        self.assertTrue(os.path.isfile(manifest_path), "scripts/dependencies.json must exist")
        with open(manifest_path, "r", encoding="utf-8") as f:
            manifest = json.load(f)
        self.assertEqual(manifest.get("version"), 1)
        self.assertIn("toolchain", manifest)
        self.assertIn("vendoredFiles", manifest)
        for entry in manifest["vendoredFiles"]:
            if entry.get("category") == "required":
                file_path = os.path.join(self.workspace_root, *entry["path"].split("/"))
                self.assertTrue(os.path.isfile(file_path), f"Required file {entry['path']} must exist")
                with open(file_path, "rb") as f:
                    content = f.read()
                actual_hash = hashlib.sha256(content).hexdigest().lower()
                self.assertEqual(
                    actual_hash,
                    entry["sha256"].lower(),
                    f"Hash mismatch for {entry['path']}: expected {entry['sha256']}, got {actual_hash}"
                )
                self.assertEqual(
                    len(content),
                    entry["size"],
                    f"Size mismatch for {entry['path']}: expected {entry['size']}, got {len(content)}"
                )

    def test_version_json_authoritative_schema_and_closure(self):
        """Authoritative version.json must exist, be schema-valid, and match all configured sources."""
        import json
        version_json_path = os.path.join(self.workspace_root, "version.json")
        self.assertTrue(os.path.isfile(version_json_path), "version.json must exist at repository root")

        with open(version_json_path, "r", encoding="utf-8") as f:
            v_data = json.load(f)

        required_fields = [
            "major", "minor", "patch", "build", "version", "versionString",
            "displayVersion", "semVer", "companyName", "fileDescription",
            "fileVersion", "internalName", "legalCopyright", "originalFilename",
            "productName", "productVersion", "comments", "language", "codePage",
            "translation", "languageId", "codePageId"
        ]
        for field in required_fields:
            self.assertIn(field, v_data, f"version.json missing required field: {field}")

        expected_ver_str = f"{v_data['major']}.{v_data['minor']}.{v_data['patch']}.{v_data['build']}"
        self.assertEqual(v_data["versionString"], expected_ver_str)
        self.assertEqual(v_data["fileVersion"], expected_ver_str)
        self.assertEqual(v_data["productVersion"], expected_ver_str)

        # Verify template source files exist
        version_in_path = os.path.join(self.workspace_root, "src", "Version.h.in")
        manifest_in_path = os.path.join(self.workspace_root, "src", "app.manifest.in")
        resources_in_path = os.path.join(self.workspace_root, "src", "resources.rc.in")
        self.assertTrue(os.path.isfile(version_in_path), "src/Version.h.in must exist")
        self.assertTrue(os.path.isfile(manifest_in_path), "src/app.manifest.in must exist")
        self.assertTrue(os.path.isfile(resources_in_path), "src/resources.rc.in must exist")

        # Verify source tree is clean of generated outputs
        self.assertFalse(os.path.isfile(os.path.join(self.workspace_root, "src", "Version.h")), "src/Version.h must not be in source tree")
        self.assertFalse(os.path.isfile(os.path.join(self.workspace_root, "src", "app.manifest")), "src/app.manifest must not be in source tree")
        self.assertFalse(os.path.isfile(os.path.join(self.workspace_root, "src", "resources.rc")), "src/resources.rc must not be in source tree")

        # Verify translation key matches language + codepage concatenation
        expected_trans = (v_data["language"] + v_data["codePage"]).lower()
        self.assertEqual(v_data["translation"].lower(), expected_trans)
    def test_pe_version_resource_inspection(self):
        """Aperture executable must contain deterministic VS_VERSION_INFO matching version.json."""
        target = self.verify_exe_path if os.path.isfile(self.verify_exe_path) else self.exe_path
        if not os.path.isfile(target):
            self.skipTest("No Aperture executable found in workspace.")

        import json
        version_json_path = os.path.join(self.workspace_root, "version.json")
        with open(version_json_path, "r", encoding="utf-8") as f:
            v_data = json.load(f)

        inspector = PEInspector(target)
        report = inspector.parse()

        self.assertIn("versionInfo", report, f"{target} parse report must contain versionInfo")
        v_info = report["versionInfo"]
        self.assertIsNotNone(v_info, f"{target} must embed a valid VS_VERSION_INFO resource")

        fixed = v_info.get("fixedFileInfo", {})
        self.assertEqual(fixed.get("signature"), "0xfeef04bd", "FixedFileInfo signature must be 0xFEEF04BD")
        self.assertEqual(
            fixed.get("fileVersion"),
            (v_data["major"], v_data["minor"], v_data["patch"], v_data["build"]),
            "FixedFileInfo fileVersion numeric tuple must match version.json"
        )
        self.assertEqual(
            fixed.get("productVersion"),
            (v_data["major"], v_data["minor"], v_data["patch"], v_data["build"]),
            "FixedFileInfo productVersion numeric tuple must match version.json"
        )

        str_info = v_info.get("stringFileInfo", {})
        self.assertGreater(len(str_info), 0, "StringFileInfo table must not be empty")

        translation_key = v_data.get("language", "0409") + v_data.get("codePage", "04b0")
        matching_table = None
        for table_key, strings in str_info.items():
            if table_key.lower() == translation_key.lower():
                matching_table = strings
                break
        if matching_table is None and len(str_info) == 1:
            matching_table = next(iter(str_info.values()))

        self.assertIsNotNone(matching_table, f"StringFileInfo table for {translation_key} must be present")
        self.assertEqual(matching_table.get("CompanyName"), v_data["companyName"])
        self.assertEqual(matching_table.get("FileDescription"), v_data["fileDescription"])
        self.assertEqual(matching_table.get("FileVersion"), v_data["fileVersion"])
        self.assertEqual(matching_table.get("InternalName"), v_data["internalName"])
        self.assertEqual(matching_table.get("LegalCopyright"), v_data["legalCopyright"])
        self.assertEqual(matching_table.get("OriginalFilename"), v_data["originalFilename"])
        self.assertEqual(matching_table.get("ProductName"), v_data["productName"])
        self.assertEqual(matching_table.get("ProductVersion"), v_data["productVersion"])

        var_info = v_info.get("varFileInfo", [])
        self.assertGreater(len(var_info), 0, "VarFileInfo translation array must not be empty")
        self.assertEqual(var_info[0]["langInt"], v_data["languageId"])
        self.assertEqual(var_info[0]["codePageInt"], v_data["codePageId"])

    def test_pe_version_resource_synthetic_decoder(self):
        """Test _decode_vs_version_info decoder on a known synthetic VS_VERSION_INFO binary buffer."""
        # Build a valid synthetic binary VS_VERSIONINFO buffer
        # Root: VS_VERSION_INFO (wLength, wValueLength=52, wType=0, szKey=VS_VERSION_INFO\0, FixedFileInfo, StringFileInfo, VarFileInfo)
        key_bytes = "VS_VERSION_INFO\0".encode("utf-16le")
        header_len = 6 + len(key_bytes)
        aligned_fixed_pos = (header_len + 3) & ~3
        pad1 = b"\0" * (aligned_fixed_pos - header_len)

        # FixedFileInfo (52 bytes)
        fixed_bytes = struct.pack(
            "<IIIIIIIIIIIII",
            0xFEEF04BD,  # dwSignature
            0x00010000,  # dwStrucVersion
            (2 << 16) | 0,  # dwFileVersionMS (2.0)
            (0 << 16) | 0,  # dwFileVersionLS (0.0)
            (2 << 16) | 0,  # dwProductVersionMS (2.0)
            (0 << 16) | 0,  # dwProductVersionLS (0.0)
            0x3F, 0x00, 0x40004, 0x01, 0x00, 0x00, 0x00
        )

        # Build String: CompanyName = "Aperture Player Project"
        s_key = "CompanyName\0".encode("utf-16le")
        s_val = "Aperture Player Project\0".encode("utf-16le")
        s_val_wchars = len(s_val) // 2
        s_hdr = 6 + len(s_key)
        s_aligned_val = (s_hdr + 3) & ~3
        s_pad = b"\0" * (s_aligned_val - s_hdr)
        s_total = s_aligned_val + len(s_val)
        s_total_aligned = (s_total + 3) & ~3
        s_trail = b"\0" * (s_total_aligned - s_total)
        s_buf = struct.pack("<HHH", s_total_aligned, s_val_wchars, 1) + s_key + s_pad + s_val + s_trail

        # StringTable: "040904b0"
        st_key = "040904b0\0".encode("utf-16le")
        st_hdr = 6 + len(st_key)
        st_aligned = (st_hdr + 3) & ~3
        st_pad = b"\0" * (st_aligned - st_hdr)
        st_total = st_aligned + len(s_buf)
        st_total_aligned = (st_total + 3) & ~3
        st_trail = b"\0" * (st_total_aligned - st_total)
        st_buf = struct.pack("<HHH", st_total_aligned, 0, 1) + st_key + st_pad + s_buf + st_trail

        # StringFileInfo
        sfi_key = "StringFileInfo\0".encode("utf-16le")
        sfi_hdr = 6 + len(sfi_key)
        sfi_aligned = (sfi_hdr + 3) & ~3
        sfi_pad = b"\0" * (sfi_aligned - sfi_hdr)
        sfi_total = sfi_aligned + len(st_buf)
        sfi_total_aligned = (sfi_total + 3) & ~3
        sfi_trail = b"\0" * (sfi_total_aligned - sfi_total)
        sfi_buf = struct.pack("<HHH", sfi_total_aligned, 0, 1) + sfi_key + sfi_pad + st_buf + sfi_trail

        # VarFileInfo -> Translation (0x0409, 0x04B0)
        var_key = "Translation\0".encode("utf-16le")
        var_val = struct.pack("<HH", 0x0409, 0x04B0)
        var_hdr = 6 + len(var_key)
        var_aligned = (var_hdr + 3) & ~3
        var_pad = b"\0" * (var_aligned - var_hdr)
        var_total = var_aligned + len(var_val)
        var_total_aligned = (var_total + 3) & ~3
        var_trail = b"\0" * (var_total_aligned - var_total)
        var_buf = struct.pack("<HHH", var_total_aligned, 4, 0) + var_key + var_pad + var_val + var_trail

        vfi_key = "VarFileInfo\0".encode("utf-16le")
        vfi_hdr = 6 + len(vfi_key)
        vfi_aligned = (vfi_hdr + 3) & ~3
        vfi_pad = b"\0" * (vfi_aligned - vfi_hdr)
        vfi_total = vfi_aligned + len(var_buf)
        vfi_total_aligned = (vfi_total + 3) & ~3
        vfi_trail = b"\0" * (vfi_total_aligned - vfi_total)
        vfi_buf = struct.pack("<HHH", vfi_total_aligned, 0, 1) + vfi_key + vfi_pad + var_buf + vfi_trail

        root_body = key_bytes + pad1 + fixed_bytes + sfi_buf + vfi_buf
        root_len = 6 + len(root_body)
        full_synthetic_buf = struct.pack("<HHH", root_len, 52, 0) + root_body

        # Instantiate dummy inspector and decode synthetic buffer
        insp = PEInspector.__new__(PEInspector)
        decoded = insp._decode_vs_version_info(full_synthetic_buf)

        self.assertIsNotNone(decoded)
        self.assertEqual(decoded["fixedFileInfo"]["signature"], "0xfeef04bd")
        self.assertEqual(decoded["fixedFileInfo"]["fileVersion"], (2, 0, 0, 0))
        self.assertEqual(decoded["fixedFileInfo"]["productVersion"], (2, 0, 0, 0))
        self.assertIn("040904b0", decoded["stringFileInfo"])
        self.assertEqual(decoded["stringFileInfo"]["040904b0"]["CompanyName"], "Aperture Player Project")
        self.assertEqual(len(decoded["varFileInfo"]), 1)
        self.assertEqual(decoded["varFileInfo"][0]["langInt"], 0x0409)
        self.assertEqual(decoded["varFileInfo"][0]["codePageInt"], 0x04B0)

class TestReproducibilityComparator(unittest.TestCase):
    """Test suite for PE detailed inspection and reproducibility normalization comparison."""

    @classmethod
    def setUpClass(cls):
        cls.workspace_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
        cls.mpv_path = os.path.join(cls.workspace_root, "libmpv-2.dll")

    def test_libmpv_identical_comparison(self):
        """Comparing libmpv-2.dll against itself must yield REPRODUCIBLE_BIT_EXACT."""
        if not os.path.isfile(self.mpv_path):
            self.skipTest("libmpv-2.dll not found in workspace.")

        insp_a = PEDetailedInspector(self.mpv_path)
        rep_a = insp_a.parse()
        insp_b = PEDetailedInspector(self.mpv_path)
        rep_b = insp_b.parse()

        comp = ReproducibilityComparator(rep_a, rep_b)
        res = comp.compare()

        self.assertTrue(res["isReproducible"])
        self.assertTrue(res["exactRawMatch"])
        self.assertEqual(res["verdict"], "REPRODUCIBLE_BIT_EXACT")
        self.assertEqual(len(res["meaningfulDifferences"]), 0)
        self.assertEqual(len(res["volatileDifferences"]), 0)

    def test_synthetic_pe_volatile_timestamp_normalization(self):
        """PE binary with only volatile timestamp / checksum diffs must yield REPRODUCIBLE_NORMALIZED."""
        if not os.path.isfile(self.mpv_path):
            self.skipTest("libmpv-2.dll not found in workspace.")

        with open(self.mpv_path, "rb") as f:
            orig_data = f.read()

        dummy_a = os.path.join(os.path.dirname(__file__), "test_dummy_pe_a.dll")
        dummy_b = os.path.join(os.path.dirname(__file__), "test_dummy_pe_b.dll")

        try:
            # Build A: original data
            with open(dummy_a, "wb") as f:
                f.write(orig_data)

            # Build B: mutate only COFF TimeDateStamp at pe_offset + 8
            pe_offset = struct.unpack_from("<I", orig_data, 0x3C)[0]
            mutated = bytearray(orig_data)
            curr_ts = struct.unpack_from("<I", mutated, pe_offset + 8)[0]
            struct.pack_into("<I", mutated, pe_offset + 8, curr_ts ^ 0x12345678)

            with open(dummy_b, "wb") as f:
                f.write(mutated)

            insp_a = PEDetailedInspector(dummy_a)
            rep_a = insp_a.parse()
            insp_b = PEDetailedInspector(dummy_b)
            rep_b = insp_b.parse()

            comp = ReproducibilityComparator(rep_a, rep_b)
            res = comp.compare()

            self.assertTrue(res["isReproducible"], "Volatile timestamp difference must remain reproducible")
            self.assertFalse(res["exactRawMatch"], "Raw hashes must differ")
            self.assertTrue(res["normalizedMatch"], "Normalized hashes must match")
            self.assertEqual(res["verdict"], "REPRODUCIBLE_NORMALIZED")
            self.assertEqual(len(res["meaningfulDifferences"]), 0)
            self.assertGreater(len(res["volatileDifferences"]), 0)
        finally:
            if os.path.exists(dummy_a):
                os.remove(dummy_a)
            if os.path.exists(dummy_b):
                os.remove(dummy_b)

    def test_synthetic_pe_code_divergence_fails(self):
        """PE binary with executable code diff in .text must yield NON_REPRODUCIBLE_DIVERGENCE."""
        if not os.path.isfile(self.mpv_path):
            self.skipTest("libmpv-2.dll not found in workspace.")

        with open(self.mpv_path, "rb") as f:
            orig_data = f.read()

        dummy_a = os.path.join(os.path.dirname(__file__), "test_dummy_code_a.dll")
        dummy_b = os.path.join(os.path.dirname(__file__), "test_dummy_code_b.dll")

        try:
            with open(dummy_a, "wb") as f:
                f.write(orig_data)

            # Mutate bytes inside section data (e.g. at offset 0x1000)
            mutated = bytearray(orig_data)
            mutated[0x1000] ^= 0xFF

            with open(dummy_b, "wb") as f:
                f.write(mutated)

            insp_a = PEDetailedInspector(dummy_a)
            rep_a = insp_a.parse()
            insp_b = PEDetailedInspector(dummy_b)
            rep_b = insp_b.parse()

            comp = ReproducibilityComparator(rep_a, rep_b)
            res = comp.compare()

            self.assertFalse(res["isReproducible"], "Code divergence must fail reproducibility")
            self.assertFalse(res["exactRawMatch"])
            self.assertEqual(res["verdict"], "NON_REPRODUCIBLE_DIVERGENCE")
            self.assertGreater(len(res["meaningfulDifferences"]), 0)
        finally:
            if os.path.exists(dummy_a):
                os.remove(dummy_a)
            if os.path.exists(dummy_b):
                os.remove(dummy_b)

if __name__ == "__main__":
    unittest.main(verbosity=2)
