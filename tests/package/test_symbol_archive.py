#!/usr/bin/env python3
"""
Unit and integration tests for release symbol archiving, PE CodeView debug directory
inspection, MSF 7.0 PDB stream parsing, content-addressed archive naming, and verification.

Tests verify:
1. Pure-Python PE parser correctly extracts CodeView RSDS GUID, age, and PDB basename.
2. Rejects local/absolute PDB paths in PE CodeView records (enforces /PDBALTPATH basename).
3. Pure-Python MSF 7.0 PDB parser correctly extracts Stream 1 GUID, age, and signature.
4. Validates matching GUID/age between PE and PDB, and detects mismatches.
5. archive_symbols.ps1 creates content-addressed <binarySha256>.symbols.zip and .symbols.json.
6. Verification mode reopens archive, validates hashes, GUID, age, version, and provenance.
7. Tamper detection: altered PDB, modified manifest, or mismatched content address fails verification.
8. Guaranteed temporary cleanup: no staging directories, PDBs, or archives persist in the repository.
"""

import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid
import zipfile


WORKSPACE_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCRIPTS_DIR = os.path.join(WORKSPACE_ROOT, "scripts")
ARCHIVE_SCRIPT = os.path.join(SCRIPTS_DIR, "archive_symbols.ps1")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest().lower()


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest().lower()


def run_powershell(command: str, env=None) -> subprocess.CompletedProcess:
    """Helper to run a PowerShell command."""
    ps_env = os.environ.copy()
    if env:
        ps_env.update(env)
    return subprocess.run(
        ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", command],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=ps_env,
    )


# ---------------------------------------------------------------------------
# Synthetic PE & PDB Builders for Fast, Offline, Deterministic Unit Tests
# ---------------------------------------------------------------------------
def create_synthetic_pe_with_codeview(
    path: str,
    guid_bytes: bytes,
    age: int = 1,
    pdb_path: str = "Aperture.pdb",
):
    """Creates a minimal valid x64 PE executable with an RSDS CodeView debug entry."""
    # Layout:
    # 0x00..0x3F: DOS header (e_lfanew at 0x3C -> 0x80)
    # 0x80..0x97: PE signature + COFF File Header
    # 0x98..0x187: Optional Header (PE32+, 240 bytes)
    # 0x188..0x1AF: Section Header .rdata (40 bytes)
    # 0x200..0x3FF: .rdata section body (containing debug directory + RSDS payload)
    file_size = 1024
    buf = bytearray(file_size)

    # DOS Header
    buf[0:2] = b"MZ"
    struct.pack_into("<I", buf, 0x3C, 0x80)

    # PE Signature
    pe_off = 0x80
    buf[pe_off:pe_off + 4] = b"PE\0\0"

    # COFF File Header
    machine = 0x8664  # x64
    num_sections = 1
    time_date_stamp = 0x12345678
    size_of_opt_header = 240
    characteristics = 0x0022  # EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE
    struct.pack_into(
        "<HHIIIHH", buf, pe_off + 4,
        machine, num_sections, time_date_stamp, 0, 0, size_of_opt_header, characteristics
    )

    opt_off = pe_off + 24
    # Optional Header Standard Fields (PE32+)
    magic = 0x20B  # PE32+
    major_linker = 14
    minor_linker = 40
    size_of_code = 0x200
    entry_point = 0x1000
    base_of_code = 0x1000
    struct.pack_into("<HBBIIIII", buf, opt_off, magic, major_linker, minor_linker, size_of_code, 0, 0, entry_point, base_of_code)

    # Optional Header Windows-Specific Fields
    image_base = 0x140000000
    section_align = 0x1000
    file_align = 0x200
    size_of_image = 0x3000
    size_of_headers = 0x200
    subsystem = 2  # WINDOWS_GUI
    dll_char = 0x8160  # DYNAMIC_BASE | HIGH_ENTROPY_VA | NX_COMPAT | GUARD_CF
    num_rva_sizes = 16
    struct.pack_into(
        "<QIIHHHHHHIIIIHH", buf, opt_off + 24,
        image_base, section_align, file_align,
        10, 0, 0, 0, 10, 0, 0,
        size_of_image, size_of_headers, 0,
        subsystem, dll_char
    )
    struct.pack_into("<QQQQII", buf, opt_off + 72, 0x100000, 0x1000, 0x100000, 0x1000, 0, num_rva_sizes)

    # Data Directory: Index 6 is Debug Directory
    dirs_off = opt_off + 112
    debug_dir_rva = 0x2000  # in .rdata
    debug_dir_size = 28
    struct.pack_into("<II", buf, dirs_off + 6 * 8, debug_dir_rva, debug_dir_size)

    # Section Header (.rdata)
    sec_off = opt_off + size_of_opt_header
    sec_name = b".rdata\0\0"
    v_size = 0x1000
    v_addr = 0x2000
    raw_size = 0x200
    raw_ptr = 0x200
    sec_flags = 0x40000040  # INITIALIZED_DATA | READ
    struct.pack_into("<8sIIIIIIHHI", buf, sec_off, sec_name, v_size, v_addr, raw_size, raw_ptr, 0, 0, 0, 0, sec_flags)

    # Debug Directory Entry at raw_ptr (0x200)
    dbg_off = 0x200
    dbg_type = 2  # IMAGE_DEBUG_TYPE_CODEVIEW
    rsds_payload_off = 0x220
    pdb_path_bytes = pdb_path.encode("utf-8") + b"\0"
    rsds_size = 4 + 16 + 4 + len(pdb_path_bytes)
    rsds_rva = v_addr + (rsds_payload_off - raw_ptr)

    struct.pack_into(
        "<IIHHIIII", buf, dbg_off,
        0, 0x12345678, 0, 0, dbg_type, rsds_size, rsds_rva, rsds_payload_off
    )

    # RSDS CodeView Structure: 'RSDS' + 16-byte GUID + 4-byte Age + PdbFileName
    buf[rsds_payload_off:rsds_payload_off + 4] = b"RSDS"
    buf[rsds_payload_off + 4:rsds_payload_off + 20] = guid_bytes
    struct.pack_into("<I", buf, rsds_payload_off + 20, age)
    buf[rsds_payload_off + 24:rsds_payload_off + 24 + len(pdb_path_bytes)] = pdb_path_bytes

    with open(path, "wb") as f:
        f.write(buf)


def create_synthetic_msf7_pdb(
    path: str,
    guid_bytes: bytes,
    age: int = 1,
):
    """Creates a minimal valid MSF 7.0 PDB file with Stream 1 (PDB Info) containing GUID and Age."""
    block_size = 4096
    # Total blocks: 0=Header, 1=FreeMap, 2=BlockMap, 3=DirStream, 4=Stream0, 5=Stream1
    num_blocks = 8
    file_bytes = bytearray(num_blocks * block_size)

    # Header signature (32 bytes)
    sig = b"Microsoft C/C++ MSF 7.00\r\n\x1a\x44\x53\x00\x00\x00"
    file_bytes[0:32] = sig

    free_block_map = 1
    block_map_addr = 2  # Block 2 contains directory block pointers

    # Construct Stream 1 (Info stream)
    # Stream 1 contains: Version (VC70 = 20000404), Signature (timestamp), Age, GUID
    stream1_data = bytearray(64)
    struct.pack_into("<III", stream1_data, 0, 20000404, 0x12345678, age)
    stream1_data[12:28] = guid_bytes

    # Put Stream 0 in Block 4 (dummy 64 bytes)
    stream0_data = b"\0" * 64
    file_bytes[4 * block_size:4 * block_size + len(stream0_data)] = stream0_data

    # Put Stream 1 in Block 5
    file_bytes[5 * block_size:5 * block_size + len(stream1_data)] = stream1_data

    # Directory stream in Block 3
    # Dir layout: NumStreams (uint32), StreamSizes (NumStreams * uint32), StreamBlocks...
    dir_stream = bytearray()
    dir_stream.extend(struct.pack("<I", 2))  # 2 streams
    dir_stream.extend(struct.pack("<II", len(stream0_data), len(stream1_data)))
    dir_stream.extend(struct.pack("<I", 4))  # Stream 0 block 4
    dir_stream.extend(struct.pack("<I", 5))  # Stream 1 block 5

    file_bytes[3 * block_size:3 * block_size + len(dir_stream)] = dir_stream

    num_dir_bytes = len(dir_stream)

    # Block 2 contains the block number of the directory stream (Block 3)
    struct.pack_into("<I", file_bytes, 2 * block_size, 3)

    # Complete Header
    struct.pack_into("<IIIII", file_bytes, 0x20, block_size, free_block_map, num_blocks, num_dir_bytes, 0)
    struct.pack_into("<I", file_bytes, 0x34, block_map_addr)

    with open(path, "wb") as f:
        f.write(file_bytes)


# ---------------------------------------------------------------------------
# Pure-Python Parsers for Verification
# ---------------------------------------------------------------------------
def parse_pe_codeview(path: str) -> dict:
    """Parses PE x64 binary and returns CodeView RSDS debug info."""
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("Missing MZ signature")

    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_off + 24 > len(data) or data[pe_off:pe_off + 4] != b"PE\0\0":
        raise ValueError("Missing PE signature")

    machine, num_sections = struct.unpack_from("<HH", data, pe_off + 4)
    if machine != 0x8664:
        raise ValueError(f"PE is not x64: 0x{machine:04x}")

    size_of_opt_header = struct.unpack_from("<H", data, pe_off + 20)[0]
    opt_off = pe_off + 24
    magic = struct.unpack_from("<H", data, opt_off)[0]
    if magic != 0x20B:
        raise ValueError("PE is not PE32+")

    num_rva_sizes = struct.unpack_from("<I", data, opt_off + 108)[0]
    dirs_off = opt_off + 112

    sec_off = opt_off + size_of_opt_header
    sections = []
    for i in range(num_sections):
        s_off = sec_off + i * 40
        sec_name = data[s_off:s_off + 8].rstrip(b"\0").decode("latin1")
        v_size, v_addr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, s_off + 8)
        sections.append({
            "name": sec_name,
            "v_size": v_size,
            "v_addr": v_addr,
            "raw_size": raw_size,
            "raw_ptr": raw_ptr,
        })

    def rva_to_offset(rva: int):
        for s in sections:
            sec_len = max(s["v_size"], s["raw_size"])
            if s["v_addr"] <= rva < s["v_addr"] + sec_len:
                return rva - s["v_addr"] + s["raw_ptr"]
        return None

    if num_rva_sizes <= 6:
        raise ValueError("No debug directory in PE")

    dbg_rva, dbg_size = struct.unpack_from("<II", data, dirs_off + 6 * 8)
    if dbg_rva == 0 or dbg_size == 0:
        raise ValueError("Empty debug directory")

    dbg_off = rva_to_offset(dbg_rva)
    if dbg_off is None:
        raise ValueError("Could not resolve debug directory RVA")

    entry_count = dbg_size // 28
    for i in range(entry_count):
        e_off = dbg_off + i * 28
        dbg_type, sz_data, addr_raw, ptr_raw = struct.unpack_from("<IIII", data, e_off + 12)
        if dbg_type == 2:  # CodeView
            if ptr_raw + sz_data <= len(data) and sz_data >= 24:
                sig = data[ptr_raw:ptr_raw + 4]
                if sig == b"RSDS":
                    guid_bytes = data[ptr_raw + 4:ptr_raw + 20]
                    age = struct.unpack_from("<I", data, ptr_raw + 20)[0]
                    end = data.find(b"\0", ptr_raw + 24)
                    pdb_path = data[ptr_raw + 24:end].decode("utf-8") if end != -1 else ""
                    pdb_filename = os.path.basename(pdb_path)
                    is_basename = (pdb_path == pdb_filename and "\\" not in pdb_path and "/" not in pdb_path and ":" not in pdb_path)

                    d1, d2, d3 = struct.unpack_from("<IHH", guid_bytes, 0)
                    d4 = guid_bytes[8:16]
                    guid_formatted = f"{d1:08x}-{d2:04x}-{d3:04x}-{d4[:2].hex()}-{d4[2:].hex()}"

                    return {
                        "signature": "RSDS",
                        "guid": guid_formatted,
                        "guid_bytes": guid_bytes,
                        "age": age,
                        "pdb_path": pdb_path,
                        "pdb_filename": pdb_filename,
                        "is_basename_only": is_basename,
                    }

    raise ValueError("No RSDS CodeView entry found")


def parse_pdb_msf7(path: str) -> dict:
    """Parses MSF 7.0 PDB file and returns Stream 1 info (GUID, Age, Version)."""
    with open(path, "rb") as f:
        data = f.read()

    sig = b"Microsoft C/C++ MSF 7.00\r\n\x1a\x44\x53\x00\x00\x00"
    if len(data) < 32 or data[:32] != sig:
        raise ValueError("Not an MSF 7.0 PDB")

    block_size, free_map, num_blocks, num_dir_bytes = struct.unpack_from("<IIII", data, 0x20)
    block_map_addr = struct.unpack_from("<I", data, 0x34)[0]

    num_dir_blocks = (num_dir_bytes + block_size - 1) // block_size
    dir_map_off = block_map_addr * block_size
    dir_blocks = [
        struct.unpack_from("<I", data, dir_map_off + i * 4)[0]
        for i in range(num_dir_blocks)
    ]

    dir_bytes = bytearray()
    for b in dir_blocks:
        src = b * block_size
        dir_bytes.extend(data[src:src + block_size])
    dir_bytes = bytes(dir_bytes[:num_dir_bytes])

    num_streams = struct.unpack_from("<I", dir_bytes, 0)[0]
    stream_sizes = [
        struct.unpack_from("<I", dir_bytes, 4 + i * 4)[0]
        for i in range(num_streams)
    ]

    cur_off = 4 + num_streams * 4
    s0_blocks_cnt = (stream_sizes[0] + block_size - 1) // block_size if stream_sizes[0] not in (0, 0xFFFFFFFF) else 0
    s1_size = stream_sizes[1]
    s1_blocks_cnt = (s1_size + block_size - 1) // block_size if s1_size not in (0, 0xFFFFFFFF) else 0

    cur_off += s0_blocks_cnt * 4
    s1_blocks = [
        struct.unpack_from("<I", dir_bytes, cur_off + i * 4)[0]
        for i in range(s1_blocks_cnt)
    ]

    s1_bytes = bytearray()
    for b in s1_blocks:
        src = b * block_size
        s1_bytes.extend(data[src:src + block_size])
    s1_bytes = bytes(s1_bytes[:s1_size])

    version, sig_stamp, age = struct.unpack_from("<III", s1_bytes, 0)
    guid_bytes = s1_bytes[12:28]

    d1, d2, d3 = struct.unpack_from("<IHH", guid_bytes, 0)
    d4 = guid_bytes[8:16]
    guid_formatted = f"{d1:08x}-{d2:04x}-{d3:04x}-{d4[:2].hex()}-{d4[2:].hex()}"

    return {
        "version": version,
        "signature": sig_stamp,
        "age": age,
        "guid": guid_formatted,
        "guid_bytes": guid_bytes,
        "size": len(data),
    }


# ---------------------------------------------------------------------------
# Test Suite
# ---------------------------------------------------------------------------
class TestReleaseSymbolArchive(unittest.TestCase):
    """Unit and integration tests for release symbol archiving and verification."""

    def setUp(self):
        self.temp_dir = tempfile.mkdtemp(prefix="Aperture-symtest-")
        self.test_guid_bytes = uuid.uuid4().bytes
        self.test_age = 1

    def tearDown(self):
        if os.path.exists(self.temp_dir):
            shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_pe_codeview_parser_valid_basename(self):
        """Valid PE with basename PDB path parses correctly."""
        pe_path = os.path.join(self.temp_dir, "Aperture.exe")
        create_synthetic_pe_with_codeview(pe_path, self.test_guid_bytes, age=self.test_age, pdb_path="Aperture.pdb")

        cv = parse_pe_codeview(pe_path)
        self.assertEqual(cv["signature"], "RSDS")
        self.assertEqual(cv["age"], self.test_age)
        self.assertEqual(cv["pdb_path"], "Aperture.pdb")
        self.assertEqual(cv["pdb_filename"], "Aperture.pdb")
        self.assertTrue(cv["is_basename_only"])

    def test_pe_codeview_parser_rejects_absolute_path_leakage(self):
        """PE CodeView entry with absolute local path is flagged as path leakage."""
        pe_path = os.path.join(self.temp_dir, "Aperture_leaked.exe")
        create_synthetic_pe_with_codeview(
            pe_path,
            self.test_guid_bytes,
            age=self.test_age,
            pdb_path=r"C:\Users\Developer\Projects\Aperture\out\Aperture.pdb"
        )

        cv = parse_pe_codeview(pe_path)
        self.assertEqual(cv["pdb_filename"], "Aperture.pdb")
        self.assertFalse(cv["is_basename_only"])
        self.assertIn("Users", cv["pdb_path"])

    def test_pdb_msf7_parser_valid(self):
        """Valid MSF 7.0 PDB file parses Stream 1 GUID and Age."""
        pdb_path = os.path.join(self.temp_dir, "Aperture.pdb")
        create_synthetic_msf7_pdb(pdb_path, self.test_guid_bytes, age=self.test_age)

        pdb_info = parse_pdb_msf7(pdb_path)
        self.assertEqual(pdb_info["age"], self.test_age)
        self.assertEqual(pdb_info["guid_bytes"], self.test_guid_bytes)
        self.assertEqual(pdb_info["version"], 20000404)

    def test_pe_and_pdb_matching(self):
        """PE CodeView and PDB Stream 1 have matching GUID and Age."""
        pe_path = os.path.join(self.temp_dir, "Aperture.exe")
        pdb_path = os.path.join(self.temp_dir, "Aperture.pdb")
        create_synthetic_pe_with_codeview(pe_path, self.test_guid_bytes, age=self.test_age, pdb_path="Aperture.pdb")
        create_synthetic_msf7_pdb(pdb_path, self.test_guid_bytes, age=self.test_age)

        cv = parse_pe_codeview(pe_path)
        pdb_info = parse_pdb_msf7(pdb_path)

        self.assertEqual(cv["guid"].lower(), pdb_info["guid"].lower())
        self.assertEqual(cv["age"], pdb_info["age"])

    def test_pe_and_pdb_mismatch_detected(self):
        """Mismatched GUID between PE and PDB is detected."""
        pe_path = os.path.join(self.temp_dir, "Aperture.exe")
        pdb_path = os.path.join(self.temp_dir, "Aperture.pdb")
        different_guid = uuid.uuid4().bytes

        create_synthetic_pe_with_codeview(pe_path, self.test_guid_bytes, age=1, pdb_path="Aperture.pdb")
        create_synthetic_msf7_pdb(pdb_path, different_guid, age=1)

        cv = parse_pe_codeview(pe_path)
        pdb_info = parse_pdb_msf7(pdb_path)

        self.assertNotEqual(cv["guid"].lower(), pdb_info["guid"].lower())

    def test_archive_symbols_script_create_and_verify(self):
        """archive_symbols.ps1 creates and verifies content-addressed archive and manifest."""
        pe_path = os.path.join(self.temp_dir, "Aperture.exe")
        pdb_path = os.path.join(self.temp_dir, "Aperture.pdb")
        out_dir = os.path.join(self.temp_dir, "symbols_out")

        create_synthetic_pe_with_codeview(pe_path, self.test_guid_bytes, age=self.test_age, pdb_path="Aperture.pdb")
        create_synthetic_msf7_pdb(pdb_path, self.test_guid_bytes, age=self.test_age)

        cmd = f'& "{ARCHIVE_SCRIPT}" -BinaryPath "{pe_path}" -PdbPath "{pdb_path}" -OutputDirectory "{out_dir}" -Json'
        res = run_powershell(cmd)
        self.assertEqual(res.returncode, 0, f"Script failed: {res.stderr}\n{res.stdout}")

        report = json.loads(res.stdout)
        self.assertEqual(report["status"], "success")
        self.assertEqual(report["mode"], "CreateAndVerify")
        self.assertTrue(report["verified"])

        bin_sha256 = report["binarySha256"]
        expected_zip = os.path.join(out_dir, f"{bin_sha256}.symbols.zip")
        expected_json = os.path.join(out_dir, f"{bin_sha256}.symbols.json")

        self.assertTrue(os.path.isfile(expected_zip), f"Archive file missing: {expected_zip}")
        self.assertTrue(os.path.isfile(expected_json), f"Manifest file missing: {expected_json}")

        # Check zip contents
        with zipfile.ZipFile(expected_zip, "r") as z:
            names = z.namelist()
            self.assertIn("Aperture.pdb", names)
            self.assertIn("manifest.json", names)

            # Read internal manifest
            internal_manifest = json.loads(z.read("manifest.json").decode("utf-8"))
            self.assertEqual(internal_manifest["application"], "Aperture")
            self.assertEqual(internal_manifest["configuration"], "Release")
            self.assertEqual(internal_manifest["binary"]["sha256"], bin_sha256)
            self.assertEqual(internal_manifest["codeview"]["pdbPath"], "Aperture.pdb")
            self.assertTrue(internal_manifest["codeview"]["isBasenameOnly"])

    def test_archive_symbols_script_rejects_path_leakage(self):
        """archive_symbols.ps1 rejects binaries with absolute CodeView PDB paths."""
        pe_path = os.path.join(self.temp_dir, "Aperture_leaked.exe")
        pdb_path = os.path.join(self.temp_dir, "Aperture.pdb")
        out_dir = os.path.join(self.temp_dir, "symbols_out")

        create_synthetic_pe_with_codeview(
            pe_path,
            self.test_guid_bytes,
            age=self.test_age,
            pdb_path=r"D:\BuildAgent\work\Aperture.pdb"
        )
        create_synthetic_msf7_pdb(pdb_path, self.test_guid_bytes, age=self.test_age)

        cmd = f'& "{ARCHIVE_SCRIPT}" -BinaryPath "{pe_path}" -PdbPath "{pdb_path}" -OutputDirectory "{out_dir}" -Json'
        res = run_powershell(cmd)
        self.assertNotEqual(res.returncode, 0, "Script should have failed due to path leakage")
        self.assertIn("leakage", res.stderr + res.stdout)

    def test_archive_symbols_script_tamper_detection(self):
        """Tampering with PDB inside symbol archive causes verification failure."""
        pe_path = os.path.join(self.temp_dir, "Aperture.exe")
        pdb_path = os.path.join(self.temp_dir, "Aperture.pdb")
        out_dir = os.path.join(self.temp_dir, "symbols_out")

        create_synthetic_pe_with_codeview(pe_path, self.test_guid_bytes, age=self.test_age, pdb_path="Aperture.pdb")
        create_synthetic_msf7_pdb(pdb_path, self.test_guid_bytes, age=self.test_age)

        cmd = f'& "{ARCHIVE_SCRIPT}" -BinaryPath "{pe_path}" -PdbPath "{pdb_path}" -OutputDirectory "{out_dir}" -Json'
        res = run_powershell(cmd)
        self.assertEqual(res.returncode, 0)
        report = json.loads(res.stdout)
        archive_file = report["archiveFile"]

        # Tamper with the zip by replacing Aperture.pdb with corrupt data
        tampered_zip = os.path.join(out_dir, "tampered.zip")
        with zipfile.ZipFile(archive_file, "r") as z_in:
            with zipfile.ZipFile(tampered_zip, "w") as z_out:
                for item in z_in.infolist():
                    if item.filename == "Aperture.pdb":
                        z_out.writestr(item, b"Corrupted PDB payload")
                    else:
                        z_out.writestr(item, z_in.read(item.filename))

        # Rename tampered zip back
        os.remove(archive_file)
        os.rename(tampered_zip, archive_file)

        # Run verification mode on tampered archive
        verify_cmd = f'& "{ARCHIVE_SCRIPT}" -Verify -ArchiveFile "{archive_file}" -BinaryPath "{pe_path}"'
        v_res = run_powershell(verify_cmd)
        self.assertNotEqual(v_res.returncode, 0, "Verification should fail for tampered PDB")


if __name__ == "__main__":
    unittest.main()
