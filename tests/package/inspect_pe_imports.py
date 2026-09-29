#!/usr/bin/env python3
"""
PE Dependency and Import Inspector for Aperture.

Parses Portable Executable (PE) headers and import directories (direct imports and
delay-loaded imports) for Win32/Win64 binaries using pure Python standard library
modules (struct, os, sys, json).

Produces structured dependency evidence for package verification and CI checks.
"""

import argparse
import json
import os
import struct
import sys
from typing import Any, Dict, List, Optional, Tuple


# Known PE Machine Types
MACHINE_NAMES = {
    0x014C: "i386",
    0x0200: "ia64",
    0x8664: "x64",
    0xAA64: "arm64",
    0x01C0: "arm",
    0x01C4: "armv7",
}

# Known Subsystem Types
SUBSYSTEM_NAMES = {
    1: "NATIVE",
    2: "WINDOWS_GUI",
    3: "WINDOWS_CUI",
    5: "OS2_CUI",
    7: "POSIX_CUI",
    9: "WINDOWS_CE_GUI",
    10: "EFI_APPLICATION",
    11: "EFI_BOOT_SERVICE_DRIVER",
    12: "EFI_RUNTIME_DRIVER",
    13: "EFI_ROM",
    14: "XBOX",
    16: "WINDOWS_BOOT_APPLICATION",
}

# Standard MSVC dynamic runtime DLLs that must NOT be present in statically-linked release builds
FORBIDDEN_DYNAMIC_RUNTIMES = {
    "MSVCP140.DLL",
    "MSVCP140D.DLL",
    "MSVCP140_1.DLL",
    "MSVCP140_2.DLL",
    "MSVCP140_CODECVT_IDS.DLL",
    "VCRUNTIME140.DLL",
    "VCRUNTIME140D.DLL",
    "VCRUNTIME140_1.DLL",
    "VCRUNTIME140_1D.DLL",
    "UCRTBASED.DLL",
}


class PEInspectionError(Exception):
    """Raised when PE parsing or validation fails."""
    pass


class PEInspector:
    """Pure Python standard library PE header and import table parser."""

    def __init__(self, file_path: str):
        self.file_path = os.path.abspath(file_path)
        self.file_name = os.path.basename(file_path)
        if not os.path.isfile(self.file_path):
            raise PEInspectionError(f"File not found: {self.file_path}")
        with open(self.file_path, "rb") as f:
            self.data = f.read()

    def parse(self) -> Dict[str, Any]:
        """Parse PE headers, sections, and import tables."""
        data = self.data
        if len(data) < 64 or data[:2] != b"MZ":
            raise PEInspectionError(f"{self.file_path} is not a valid MZ executable (missing MZ signature)")

        pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
        if pe_offset + 24 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise PEInspectionError(f"{self.file_path} is not a valid PE executable (missing PE signature)")

        # COFF File Header
        machine, num_sections, time_date_stamp, ptr_symbols, num_symbols, size_of_opt_header, characteristics = (
            struct.unpack_from("<HHIIIHH", data, pe_offset + 4)
        )

        opt_header_offset = pe_offset + 24
        if opt_header_offset + size_of_opt_header > len(data):
            raise PEInspectionError(f"{self.file_path} optional header exceeds file size")

        magic = struct.unpack_from("<H", data, opt_header_offset)[0]
        is_64bit = (magic == 0x20B)
        is_32bit = (magic == 0x10B)

        if not (is_64bit or is_32bit):
            raise PEInspectionError(f"{self.file_path} unknown optional header magic: 0x{magic:04x}")

        # Subsystem and Data Directory offsets
        # PE32: Subsystem at opt_header + 68, NumberOfRvaAndSizes at opt_header + 92, DataDirectories at opt_header + 96
        # PE32+: Subsystem at opt_header + 68, NumberOfRvaAndSizes at opt_header + 108, DataDirectories at opt_header + 112
        subsystem_offset = opt_header_offset + 68
        subsystem = 0
        if subsystem_offset + 2 <= opt_header_offset + size_of_opt_header:
            subsystem = struct.unpack_from("<H", data, subsystem_offset)[0]

        if is_64bit:
            num_rva_offset = opt_header_offset + 108
            dirs_offset = opt_header_offset + 112
        else:
            num_rva_offset = opt_header_offset + 92
            dirs_offset = opt_header_offset + 96

        num_rva_and_sizes = 0
        if num_rva_offset + 4 <= opt_header_offset + size_of_opt_header:
            num_rva_and_sizes = struct.unpack_from("<I", data, num_rva_offset)[0]

        # Section Headers
        section_headers_offset = opt_header_offset + size_of_opt_header
        sections: List[Dict[str, Any]] = []
        for i in range(num_sections):
            sec_offset = section_headers_offset + i * 40
            if sec_offset + 40 > len(data):
                break
            sec_name = data[sec_offset:sec_offset + 8].rstrip(b"\0").decode("latin-1", errors="ignore")
            vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, sec_offset + 8)
            sections.append({
                "name": sec_name,
                "vsize": vsize,
                "vaddr": vaddr,
                "raw_size": raw_size,
                "raw_ptr": raw_ptr,
            })

        def rva_to_offset(rva: int) -> Optional[int]:
            if rva == 0:
                return None
            for s in sections:
                sec_len = max(s["vsize"], s["raw_size"])
                if s["vaddr"] <= rva < s["vaddr"] + sec_len:
                    offset = rva - s["vaddr"] + s["raw_ptr"]
                    if offset < len(data):
                        return offset
            return None

        # Data Directory index 1: Import Table
        imports: List[str] = []
        if num_rva_and_sizes > 1:
            import_dir_entry = dirs_offset + 1 * 8
            if import_dir_entry + 8 <= opt_header_offset + size_of_opt_header:
                import_rva, import_size = struct.unpack_from("<II", data, import_dir_entry)
                if import_rva != 0:
                    import_desc_offset = rva_to_offset(import_rva)
                    if import_desc_offset is not None:
                        while import_desc_offset + 20 <= len(data):
                            orig_first_thunk, time_stamp, forwarder, name_rva, first_thunk = (
                                struct.unpack_from("<IIIII", data, import_desc_offset)
                            )
                            # Terminator descriptor has name_rva == 0 and first_thunk == 0
                            if name_rva == 0 and first_thunk == 0:
                                break
                            if name_rva != 0:
                                name_offset = rva_to_offset(name_rva)
                                if name_offset is not None:
                                    end = data.find(b"\0", name_offset)
                                    if end != -1:
                                        dll_name = data[name_offset:end].decode("ascii", errors="ignore").strip()
                                        if dll_name and dll_name not in imports:
                                            imports.append(dll_name)
                            import_desc_offset += 20

        # Data Directory index 13: Delay Import Table
        delay_imports: List[str] = []
        if num_rva_and_sizes > 13:
            delay_dir_entry = dirs_offset + 13 * 8
            if delay_dir_entry + 8 <= opt_header_offset + size_of_opt_header:
                delay_rva, delay_size = struct.unpack_from("<II", data, delay_dir_entry)
                if delay_rva != 0:
                    delay_desc_offset = rva_to_offset(delay_rva)
                    if delay_desc_offset is not None:
                        while delay_desc_offset + 32 <= len(data):
                            attrs, name_rva, hmod_rva, iat_rva, int_rva, bound_iat_rva, unload_iat_rva, time_stamp = (
                                struct.unpack_from("<IIIIIIII", data, delay_desc_offset)
                            )
                            if name_rva == 0:
                                break
                            name_offset = rva_to_offset(name_rva)
                            if name_offset is not None:
                                end = data.find(b"\0", name_offset)
                                if end != -1:
                                    dll_name = data[name_offset:end].decode("ascii", errors="ignore").strip()
                                    if dll_name and dll_name not in delay_imports:
                                        delay_imports.append(dll_name)
                            delay_desc_offset += 32

        # Data Directory index 2: Resource Table (parse VS_VERSION_INFO if present)
        version_info: Optional[Dict[str, Any]] = None
        if num_rva_and_sizes > 2:
            rsrc_dir_entry = dirs_offset + 2 * 8
            if rsrc_dir_entry + 8 <= opt_header_offset + size_of_opt_header:
                rsrc_rva, rsrc_size = struct.unpack_from("<II", data, rsrc_dir_entry)
                if rsrc_rva != 0:
                    rsrc_offset = rva_to_offset(rsrc_rva)
                    if rsrc_offset is not None and rsrc_offset + 16 <= len(data):
                        version_info = self._parse_version_resource(data, rsrc_offset, rva_to_offset)

        machine_hex = f"0x{machine:04x}"
        return {
            "path": self.file_path,
            "fileName": self.file_name,
            "machine": machine_hex,
            "machineName": MACHINE_NAMES.get(machine, "unknown"),
            "is_64bit": is_64bit,
            "subsystem": subsystem,
            "subsystemName": SUBSYSTEM_NAMES.get(subsystem, "UNKNOWN"),
            "imports": imports,
            "delay_imports": delay_imports,
            "sections": [s["name"] for s in sections],
            "versionInfo": version_info,
        }

    def _parse_version_resource(
        self,
        data: bytes,
        rsrc_offset: int,
        rva_to_offset_fn: Any
    ) -> Optional[Dict[str, Any]]:
        """Extract and parse VS_VERSION_INFO resource from .rsrc directory tree."""
        if rsrc_offset + 16 > len(data):
            return None

        # Level 1: Root Directory (Resource Types)
        char, stamp, maj_v, min_v, num_named, num_id = struct.unpack_from("<IIHHHH", data, rsrc_offset)
        total_entries = num_named + num_id
        type_sub_offset = None

        for i in range(total_entries):
            entry_off = rsrc_offset + 16 + i * 8
            if entry_off + 8 > len(data):
                break
            type_id, type_data_off = struct.unpack_from("<II", data, entry_off)
            if type_id == 16 and (type_data_off & 0x80000000):  # RT_VERSION = 16
                type_sub_offset = rsrc_offset + (type_data_off & 0x7FFFFFFF)
                break

        if type_sub_offset is None or type_sub_offset + 16 > len(data):
            return None

        # Level 2: Name/ID Directory for RT_VERSION
        char, stamp, maj_v, min_v, num_named, num_id = struct.unpack_from("<IIHHHH", data, type_sub_offset)
        total_name_entries = num_named + num_id
        lang_sub_offset = None

        for j in range(total_name_entries):
            entry_off = type_sub_offset + 16 + j * 8
            if entry_off + 8 > len(data):
                break
            name_id, name_data_off = struct.unpack_from("<II", data, entry_off)
            if name_data_off & 0x80000000:
                lang_sub_offset = rsrc_offset + (name_data_off & 0x7FFFFFFF)
                break

        if lang_sub_offset is None or lang_sub_offset + 16 > len(data):
            return None

        # Level 3: Language Directory
        char, stamp, maj_v, min_v, num_named, num_id = struct.unpack_from("<IIHHHH", data, lang_sub_offset)
        total_lang_entries = num_named + num_id
        version_bytes = None

        for k in range(total_lang_entries):
            entry_off = lang_sub_offset + 16 + k * 8
            if entry_off + 8 > len(data):
                break
            lang_id, data_off = struct.unpack_from("<II", data, entry_off)
            if not (data_off & 0x80000000):
                data_entry_off = rsrc_offset + data_off
                if data_entry_off + 16 <= len(data):
                    data_rva, data_size, codepage, reserved = struct.unpack_from("<IIII", data, data_entry_off)
                    raw_off = rva_to_offset_fn(data_rva)
                    if raw_off is not None and raw_off + data_size <= len(data):
                        version_bytes = data[raw_off : raw_off + data_size]
                        break

        if not version_bytes or len(version_bytes) < 40:
            return None

        return self._decode_vs_version_info(version_bytes)

    def _decode_vs_version_info(self, raw: bytes) -> Dict[str, Any]:
        """Decode binary VS_VERSIONINFO structure into structured dictionary."""
        wLength, wValueLength, wType = struct.unpack_from("<HHH", raw, 0)
        pos = 6
        key_str = ""
        while pos + 1 < len(raw):
            wc = raw[pos:pos+2]
            pos += 2
            if wc == b"\0\0":
                break
            key_str += wc.decode("utf-16le", errors="ignore")
        pos = (pos + 3) & ~3

        fixed_info: Dict[str, Any] = {}
        if wValueLength >= 52 and pos + 52 <= len(raw):
            (sig, struc_ver,
             file_ver_ms, file_ver_ls,
             prod_ver_ms, prod_ver_ls,
             flags_mask, flags,
             file_os, file_type, file_subtype,
             date_ms, date_ls) = struct.unpack_from("<IIIIIIIIIIIII", raw, pos)

            if sig == 0xFEEF04BD:
                f_major = (file_ver_ms >> 16) & 0xFFFF
                f_minor = file_ver_ms & 0xFFFF
                f_patch = (file_ver_ls >> 16) & 0xFFFF
                f_build = file_ver_ls & 0xFFFF

                p_major = (prod_ver_ms >> 16) & 0xFFFF
                p_minor = prod_ver_ms & 0xFFFF
                p_patch = (prod_ver_ls >> 16) & 0xFFFF
                p_build = prod_ver_ls & 0xFFFF

                fixed_info = {
                    "signature": f"0x{sig:08x}",
                    "structVersion": f"{struc_ver >> 16}.{struc_ver & 0xFFFF}",
                    "fileVersion": (f_major, f_minor, f_patch, f_build),
                    "fileVersionString": f"{f_major}.{f_minor}.{f_patch}.{f_build}",
                    "productVersion": (p_major, p_minor, p_patch, p_build),
                    "productVersionString": f"{p_major}.{p_minor}.{p_patch}.{p_build}",
                    "fileFlagsMask": f"0x{flags_mask:x}",
                    "fileFlags": f"0x{flags:x}",
                    "fileOS": f"0x{file_os:05x}",
                    "fileType": f"0x{file_type:x}",
                    "fileSubtype": f"0x{file_subtype:x}",
                }
            pos += wValueLength
            pos = (pos + 3) & ~3

        string_file_info: Dict[str, Dict[str, str]] = {}
        var_file_info: List[Dict[str, Any]] = []

        while pos + 6 <= min(wLength, len(raw)):
            child_start = pos
            c_len, c_val_len, c_type = struct.unpack_from("<HHH", raw, pos)
            if c_len == 0:
                break
            pos += 6
            child_key = ""
            while pos + 1 < len(raw):
                wc = raw[pos:pos+2]
                pos += 2
                if wc == b"\0\0":
                    break
                child_key += wc.decode("utf-16le", errors="ignore")
            pos = (pos + 3) & ~3
            child_end = child_start + c_len

            if child_key == "StringFileInfo":
                while pos + 6 <= min(child_end, len(raw)):
                    st_start = pos
                    st_len, st_val_len, st_type = struct.unpack_from("<HHH", raw, pos)
                    if st_len == 0:
                        break
                    pos += 6
                    st_key = ""
                    while pos + 1 < len(raw):
                        wc = raw[pos:pos+2]
                        pos += 2
                        if wc == b"\0\0":
                            break
                        st_key += wc.decode("utf-16le", errors="ignore")
                    pos = (pos + 3) & ~3
                    st_end = st_start + st_len

                    table_strings: Dict[str, str] = {}
                    while pos + 6 <= min(st_end, len(raw)):
                        s_start = pos
                        s_len, s_val_len, s_type = struct.unpack_from("<HHH", raw, pos)
                        if s_len == 0:
                            break
                        pos += 6
                        s_key = ""
                        while pos + 1 < len(raw):
                            wc = raw[pos:pos+2]
                            pos += 2
                            if wc == b"\0\0":
                                break
                            s_key += wc.decode("utf-16le", errors="ignore")
                        pos = (pos + 3) & ~3

                        s_val = ""
                        if s_val_len > 0 and pos + s_val_len * 2 <= len(raw):
                            val_bytes = raw[pos : pos + s_val_len * 2]
                            s_val = val_bytes.decode("utf-16le", errors="ignore").rstrip("\0")
                            pos += s_val_len * 2
                        pos = (pos + 3) & ~3
                        if s_key:
                            table_strings[s_key] = s_val
                        pos = (s_start + s_len + 3) & ~3

                    string_file_info[st_key] = table_strings
                    pos = (st_end + 3) & ~3

            elif child_key == "VarFileInfo":
                while pos + 6 <= min(child_end, len(raw)):
                    v_start = pos
                    v_len, v_val_len, v_type = struct.unpack_from("<HHH", raw, pos)
                    if v_len == 0:
                        break
                    pos += 6
                    v_key = ""
                    while pos + 1 < len(raw):
                        wc = raw[pos:pos+2]
                        pos += 2
                        if wc == b"\0\0":
                            break
                        v_key += wc.decode("utf-16le", errors="ignore")
                    pos = (pos + 3) & ~3
                    v_end = v_start + v_len

                    if v_key == "Translation" and v_val_len >= 4:
                        for t_idx in range(0, v_val_len, 4):
                            if pos + t_idx + 4 <= len(raw):
                                lang_id, cp_id = struct.unpack_from("<HH", raw, pos + t_idx)
                                var_file_info.append({
                                    "langId": f"0x{lang_id:04x}",
                                    "codePageId": f"0x{cp_id:04x}",
                                    "langInt": lang_id,
                                    "codePageInt": cp_id,
                                })
                    pos = (v_end + 3) & ~3

            pos = (child_end + 3) & ~3

        return {
            "rawSize": len(raw),
            "fixedFileInfo": fixed_info,
            "stringFileInfo": string_file_info,
            "varFileInfo": var_file_info,
        }


def inspect_files(file_paths: List[str]) -> Dict[str, Any]:
    """Inspect multiple PE files and return aggregate report dict."""
    reports = []
    for path in file_paths:
        inspector = PEInspector(path)
        report = inspector.parse()
        reports.append(report)
    return {"reports": reports}


def self_test() -> bool:
    """Run verification self-tests on PE parsing logic and available binaries."""
    print("Running inspect_pe_imports self-tests...")
    # Test MZ validation failure
    dummy_data = b"NOT_A_PE_FILE_AT_ALL_1234567890"
    dummy_path = "test_dummy_invalid.bin"
    try:
        with open(dummy_path, "wb") as f:
            f.write(dummy_data)
        try:
            PEInspector(dummy_path).parse()
            print("FAIL: Expected PEInspectionError for non-PE binary")
            return False
        except PEInspectionError:
            pass
    finally:
        if os.path.exists(dummy_path):
            os.remove(dummy_path)

    # Test on workspace binaries if present
    workspace_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    test_binaries = ["Aperture.exe", "libmpv-2.dll", "Aperture.verify.exe"]
    found_any = False
    for name in test_binaries:
        full_path = os.path.join(workspace_root, name)
        if os.path.isfile(full_path):
            found_any = True
            report = PEInspector(full_path).parse()
            if report["machine"] != "0x8664":
                print(f"FAIL: {name} is not x64: {report['machine']}")
                return False
            if name.startswith("Aperture"):
                upper_imports = {imp.upper() for imp in report["imports"]}
                for forbidden in FORBIDDEN_DYNAMIC_RUNTIMES:
                    if forbidden in upper_imports:
                        print(f"FAIL: {name} imports forbidden dynamic runtime: {forbidden}")
                        return False

    if found_any:
        print("Self-test passed against workspace binaries.")
    else:
        print("Self-test passed (no workspace binaries present).")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Inspect PE architecture and import dependencies for Windows binaries."
    )
    parser.add_argument(
        "files",
        nargs="*",
        help="One or more PE binary paths (e.g. Aperture.exe libmpv-2.dll)"
    )
    parser.add_argument(
        "--json",
        action="store_true",
        default=True,
        help="Emit JSON report to stdout (default behavior)"
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run internal self-tests"
    )
    parser.add_argument(
        "--require-x64",
        action="store_true",
        help="Enforce that all inspected binaries must have machine architecture 0x8664"
    )
    parser.add_argument(
        "--check-static-runtime",
        action="store_true",
        help="Enforce that inspected binaries do not import dynamic MSVC runtime DLLs"
    )

    args = parser.parse_args()

    if args.self_test:
        return 0 if self_test() else 1

    if not args.files:
        parser.print_help(sys.stderr)
        return 1

    try:
        report_data = inspect_files(args.files)
    except Exception as e:
        sys.stderr.write(f"PE inspection error: {e}\n")
        return 1

    if args.require_x64:
        for r in report_data["reports"]:
            if r["machine"] != "0x8664":
                sys.stderr.write(f"Architecture violation: {r['path']} has machine {r['machine']} (expected 0x8664)\n")
                return 2

    if args.check_static_runtime:
        for r in report_data["reports"]:
            upper_imports = {imp.upper() for imp in r["imports"]}
            for forbidden in FORBIDDEN_DYNAMIC_RUNTIMES:
                if forbidden in upper_imports:
                    sys.stderr.write(f"Static runtime violation: {r['path']} imports dynamic runtime {forbidden}\n")
                    return 3

    print(json.dumps(report_data, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
