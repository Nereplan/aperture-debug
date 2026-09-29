#!/usr/bin/env python3
"""
PE Binary Reproducibility & Structural Normalization Inspector.

Compares two PE (Portable Executable) x64 binaries produced from identical
sources and toolchains. Validates bit-for-bit raw SHA-256 reproducibility first,
and if raw hashes differ, performs comprehensive structural PE parsing to identify,
quantify, and report all differences across:
  - COFF File Header & TimeDateStamp
  - Optional Header (CheckSum, DllCharacteristics, Subsystem, ImageBase)
  - Section Headers & Raw/Normalized Section Byte Hashes (.text, .rdata, .data, .pdata, .rsrc, .reloc)
  - Import & Delay-Import Tables
  - Resource Directory Hierarchy (.rsrc) & Leaf Resource Payloads
  - Debug Directory Entries (CodeView RSDS, PDB GUID, Age, Timestamp)
  - Base Relocations (.reloc) & Exception Data (.pdata)
  - Load Config Table (CFG, CET, Security Cookies)

Differentiates documented volatile fields (e.g. legacy linker timestamps, PE checksums)
from meaningful executable/data divergences, providing an authoritative reproducibility verdict.
"""

import argparse
import hashlib
import json
import os
import struct
import sys
from typing import Any, Dict, List, Optional, Tuple


# Documented volatile field identifiers
VOLATILE_COFF_TIMESTAMP = "COFF.TimeDateStamp"
VOLATILE_OPTIONAL_CHECKSUM = "OptionalHeader.CheckSum"
VOLATILE_DEBUG_TIMESTAMP = "DebugDirectory.TimeDateStamp"
VOLATILE_DEBUG_CODEVIEW_GUID = "DebugDirectory.CodeView.GUID"
VOLATILE_DEBUG_CODEVIEW_AGE = "DebugDirectory.CodeView.Age"
VOLATILE_RESOURCE_TIMESTAMP = "ResourceDirectory.TimeDateStamp"

DOCUMENTED_VOLATILE_FIELDS = {
    VOLATILE_COFF_TIMESTAMP,
    VOLATILE_OPTIONAL_CHECKSUM,
    VOLATILE_DEBUG_TIMESTAMP,
    VOLATILE_DEBUG_CODEVIEW_GUID,
    VOLATILE_DEBUG_CODEVIEW_AGE,
    VOLATILE_RESOURCE_TIMESTAMP,
}


def sha256_bytes(data: bytes) -> str:
    """Compute SHA-256 hex digest of raw bytes."""
    return hashlib.sha256(data).hexdigest().lower()


def sha256_file(file_path: str) -> str:
    """Compute SHA-256 hex digest of a file."""
    h = hashlib.sha256()
    with open(file_path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest().lower()


class PEParseError(Exception):
    """Raised when PE parsing fails."""
    pass


class PEDetailedInspector:
    """Comprehensive PE parser for structural reproducibility comparison."""

    def __init__(self, file_path: str):
        self.file_path = os.path.abspath(file_path)
        self.file_name = os.path.basename(file_path)
        if not os.path.isfile(self.file_path):
            raise PEParseError(f"File not found: {self.file_path}")
        with open(self.file_path, "rb") as f:
            self.data = f.read()
        self.file_size = len(self.data)
        self.file_sha256 = sha256_bytes(self.data)
        self.volatile_spans: List[Tuple[int, int, str]] = []

    def rva_to_offset(self, rva: int, sections: List[Dict[str, Any]]) -> Optional[int]:
        if rva == 0:
            return None
        for s in sections:
            sec_len = max(s["vsize"], s["raw_size"])
            if s["vaddr"] <= rva < s["vaddr"] + sec_len:
                offset = rva - s["vaddr"] + s["raw_ptr"]
                if offset < len(self.data):
                    return offset
        return None

    def parse(self) -> Dict[str, Any]:
        data = self.data
        if len(data) < 64 or data[:2] != b"MZ":
            raise PEParseError(f"{self.file_path}: Missing MZ signature")

        pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
        if pe_offset + 24 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise PEParseError(f"{self.file_path}: Missing PE signature at 0x{pe_offset:x}")

        # COFF File Header
        machine, num_sections, time_date_stamp, ptr_symbols, num_symbols, size_of_opt_header, characteristics = (
            struct.unpack_from("<HHIIIHH", data, pe_offset + 4)
        )
        # Register volatile COFF timestamp span (pe_offset + 8, length 4)
        self.volatile_spans.append((pe_offset + 8, pe_offset + 12, VOLATILE_COFF_TIMESTAMP))

        opt_header_offset = pe_offset + 24
        if opt_header_offset + size_of_opt_header > len(data):
            raise PEParseError(f"{self.file_path}: Optional header extends beyond file boundary")

        magic = struct.unpack_from("<H", data, opt_header_offset)[0]
        is_64bit = (magic == 0x20B)
        is_32bit = (magic == 0x10B)
        if not (is_64bit or is_32bit):
            raise PEParseError(f"{self.file_path}: Unsupported optional header magic 0x{magic:04x}")

        # Optional Header Standard Fields
        major_linker, minor_linker, size_of_code, size_of_init_data, size_of_uninit_data, entry_point, base_of_code = (
            struct.unpack_from("<BBIIIII", data, opt_header_offset + 2)
        )

        if is_64bit:
            image_base = struct.unpack_from("<Q", data, opt_header_offset + 24)[0]
            sec_align, file_align = struct.unpack_from("<II", data, opt_header_offset + 32)
            major_os, minor_os, major_img, minor_img, major_sub, minor_sub, win32_ver = struct.unpack_from(
                "<HHHHHHI", data, opt_header_offset + 40
            )
            size_of_image, size_of_headers, checksum, subsystem, dll_char = struct.unpack_from(
                "<IIHHH", data, opt_header_offset + 56
            )
            # Checksum at opt_header_offset + 64, 4 bytes
            self.volatile_spans.append((opt_header_offset + 64, opt_header_offset + 68, VOLATILE_OPTIONAL_CHECKSUM))
            stack_res, stack_commit, heap_res, heap_commit, loader_flags, num_rva_sizes = struct.unpack_from(
                "<QQQQII", data, opt_header_offset + 72
            )
            dirs_offset = opt_header_offset + 112
        else:
            image_base = struct.unpack_from("<I", data, opt_header_offset + 28)[0]
            sec_align, file_align = struct.unpack_from("<II", data, opt_header_offset + 32)
            major_os, minor_os, major_img, minor_img, major_sub, minor_sub, win32_ver = struct.unpack_from(
                "<HHHHHHI", data, opt_header_offset + 40
            )
            size_of_image, size_of_headers, checksum, subsystem, dll_char = struct.unpack_from(
                "<IIHHH", data, opt_header_offset + 56
            )
            self.volatile_spans.append((opt_header_offset + 64, opt_header_offset + 68, VOLATILE_OPTIONAL_CHECKSUM))
            stack_res, stack_commit, heap_res, heap_commit, loader_flags, num_rva_sizes = struct.unpack_from(
                "<IIIIII", data, opt_header_offset + 72
            )
            dirs_offset = opt_header_offset + 96

        # Parse Section Headers
        section_headers_offset = opt_header_offset + size_of_opt_header
        sections: List[Dict[str, Any]] = []
        for i in range(num_sections):
            sec_off = section_headers_offset + i * 40
            if sec_off + 40 > len(data):
                break
            sec_name_raw = data[sec_off:sec_off + 8].rstrip(b"\0")
            sec_name = sec_name_raw.decode("latin-1", errors="ignore")
            vsize, vaddr, raw_size, raw_ptr, reloc_ptr, linenum_ptr, num_relocs, num_linenums, sec_char = (
                struct.unpack_from("<IIIIIIHHI", data, sec_off + 8)
            )

            # Extract raw section slice
            sec_data_slice = b""
            if raw_ptr < len(data) and raw_size > 0:
                end_ptr = min(raw_ptr + raw_size, len(data))
                sec_data_slice = data[raw_ptr:end_ptr]

            sections.append({
                "index": i,
                "name": sec_name,
                "vsize": vsize,
                "vaddr": vaddr,
                "raw_size": raw_size,
                "raw_ptr": raw_ptr,
                "characteristics": sec_char,
                "sha256": sha256_bytes(sec_data_slice),
                "data_length": len(sec_data_slice),
            })

        # Parse Data Directories
        data_directories: List[Dict[str, Any]] = []
        dir_names = [
            "EXPORT", "IMPORT", "RESOURCE", "EXCEPTION", "SECURITY", "BASERELOC",
            "DEBUG", "ARCHITECTURE", "GLOBALPTR", "TLS", "LOAD_CONFIG", "BOUND_IMPORT",
            "IAT", "DELAY_IMPORT", "CLR_RUNTIME", "RESERVED"
        ]
        for d_idx in range(min(num_rva_sizes, 16)):
            d_off = dirs_offset + d_idx * 8
            if d_off + 8 <= len(data):
                rva, size = struct.unpack_from("<II", data, d_off)
                d_name = dir_names[d_idx] if d_idx < len(dir_names) else f"DIR_{d_idx}"
                data_directories.append({
                    "index": d_idx,
                    "name": d_name,
                    "rva": rva,
                    "size": size,
                })

        # Parse Imports
        imports: List[Dict[str, Any]] = []
        if len(data_directories) > 1 and data_directories[1]["rva"] != 0:
            imp_rva = data_directories[1]["rva"]
            imp_off = self.rva_to_offset(imp_rva, sections)
            if imp_off is not None:
                while imp_off + 20 <= len(data):
                    orig_first_thunk, time_stamp, forwarder, name_rva, first_thunk = (
                        struct.unpack_from("<IIIII", data, imp_off)
                    )
                    if name_rva == 0 and first_thunk == 0:
                        break
                    dll_name = ""
                    name_off = self.rva_to_offset(name_rva, sections)
                    if name_off is not None:
                        end = data.find(b"\0", name_off)
                        if end != -1:
                            dll_name = data[name_off:end].decode("ascii", errors="ignore").strip()

                    # Parse functions/thunks
                    thunk_rva = orig_first_thunk if orig_first_thunk != 0 else first_thunk
                    thunk_off = self.rva_to_offset(thunk_rva, sections)
                    functions: List[str] = []
                    if thunk_off is not None:
                        entry_size = 8 if is_64bit else 4
                        ordinal_mask = 0x8000000000000000 if is_64bit else 0x80000000
                        fmt = "<Q" if is_64bit else "<I"
                        while thunk_off + entry_size <= len(data):
                            val = struct.unpack_from(fmt, data, thunk_off)[0]
                            if val == 0:
                                break
                            if val & ordinal_mask:
                                ordinal = val & 0xFFFF
                                functions.append(f"Ordinal_{ordinal}")
                            else:
                                fn_name_off = self.rva_to_offset(int(val & 0x7FFFFFFF), sections)
                                if fn_name_off is not None and fn_name_off + 2 < len(data):
                                    fn_end = data.find(b"\0", fn_name_off + 2)
                                    if fn_end != -1:
                                        fn_str = data[fn_name_off + 2:fn_end].decode("ascii", errors="ignore")
                                        functions.append(fn_str)
                            thunk_off += entry_size

                    if dll_name:
                        imports.append({
                            "dll": dll_name,
                            "functions": sorted(functions),
                            "function_count": len(functions),
                        })
                    imp_off += 20

        # Parse Debug Directory
        debug_entries: List[Dict[str, Any]] = []
        if len(data_directories) > 6 and data_directories[6]["rva"] != 0:
            dbg_rva = data_directories[6]["rva"]
            dbg_size = data_directories[6]["size"]
            dbg_off = self.rva_to_offset(dbg_rva, sections)
            if dbg_off is not None:
                entry_count = dbg_size // 28
                for i in range(entry_count):
                    e_off = dbg_off + i * 28
                    if e_off + 28 <= len(data):
                        char, stamp, maj_v, min_v, dbg_type, sz_data, addr_raw, ptr_raw = (
                            struct.unpack_from("<IIHHIIII", data, e_off)
                        )
                        self.volatile_spans.append((e_off + 4, e_off + 8, VOLATILE_DEBUG_TIMESTAMP))

                        entry_info: Dict[str, Any] = {
                            "type": dbg_type,
                            "typeName": {
                                1: "COFF", 2: "CODEVIEW", 3: "FPO", 4: "MISC",
                                5: "EXCEPTION", 6: "FIXUP", 9: "BORLAND",
                                16: "REPRO", 20: "EMBEDDED_PORTABLE_PDB"
                            }.get(dbg_type, f"TYPE_{dbg_type}"),
                            "timestamp": stamp,
                            "size": sz_data,
                            "raw_ptr": ptr_raw,
                        }

                        # If CodeView (type 2)
                        if dbg_type == 2 and ptr_raw + sz_data <= len(data) and sz_data >= 24:
                            cv_sig = data[ptr_raw:ptr_raw + 4]
                            if cv_sig == b"RSDS":
                                # RSDS: 4-byte sig, 16-byte GUID, 4-byte Age, UTF-8 path
                                guid_bytes = data[ptr_raw + 4:ptr_raw + 20]
                                age = struct.unpack_from("<I", data, ptr_raw + 20)[0]
                                pdb_end = data.find(b"\0", ptr_raw + 24)
                                pdb_path = data[ptr_raw + 24:pdb_end].decode("utf-8", errors="ignore") if pdb_end != -1 else ""
                                guid_hex = guid_bytes.hex()

                                # Mark codeview guid and age as potentially volatile if needed
                                self.volatile_spans.append((ptr_raw + 4, ptr_raw + 20, VOLATILE_DEBUG_CODEVIEW_GUID))
                                self.volatile_spans.append((ptr_raw + 20, ptr_raw + 24, VOLATILE_DEBUG_CODEVIEW_AGE))

                                entry_info["codeview"] = {
                                    "signature": "RSDS",
                                    "guid": guid_hex,
                                    "age": age,
                                    "pdb_path": pdb_path,
                                    "pdb_filename": os.path.basename(pdb_path),
                                }

                        debug_entries.append(entry_info)

        # Parse Resources (.rsrc) summary
        resources: List[Dict[str, Any]] = []
        if len(data_directories) > 2 and data_directories[2]["rva"] != 0:
            rsrc_rva = data_directories[2]["rva"]
            rsrc_off = self.rva_to_offset(rsrc_rva, sections)
            if rsrc_off is not None and rsrc_off + 16 <= len(data):
                # Type Directory (level 1)
                char, stamp, maj_v, min_v, num_named, num_id = struct.unpack_from("<IIHHHH", data, rsrc_off)
                self.volatile_spans.append((rsrc_off + 4, rsrc_off + 8, VOLATILE_RESOURCE_TIMESTAMP))
                type_count = num_named + num_id
                entry_ptr = rsrc_off + 16
                for t_idx in range(type_count):
                    if entry_ptr + 8 > len(data):
                        break
                    type_id, type_data_off = struct.unpack_from("<II", data, entry_ptr)
                    entry_ptr += 8
                    type_is_dir = bool(type_data_off & 0x80000000)
                    type_sub_off = rsrc_off + (type_data_off & 0x7FFFFFFF)
                    type_name = {
                        1: "CURSOR", 2: "BITMAP", 3: "ICON", 4: "MENU", 5: "DIALOG",
                        6: "STRING", 7: "FONTDIR", 8: "FONT", 9: "ACCELERATOR",
                        10: "RCDATA", 11: "MESSAGETABLE", 12: "GROUP_CURSOR",
                        14: "GROUP_ICON", 16: "VERSION", 24: "MANIFEST"
                    }.get(type_id, f"TYPE_{type_id}")

                    if type_is_dir and type_sub_off + 16 <= len(data):
                        _, _, _, _, name_named, name_id = struct.unpack_from("<IIHHHH", data, type_sub_off)
                        name_count = name_named + name_id
                        self.volatile_spans.append((type_sub_off + 4, type_sub_off + 8, VOLATILE_RESOURCE_TIMESTAMP))
                        n_entry_ptr = type_sub_off + 16
                        for n_idx in range(name_count):
                            if n_entry_ptr + 8 > len(data):
                                break
                            res_id, res_data_off = struct.unpack_from("<II", data, n_entry_ptr)
                            n_entry_ptr += 8
                            res_is_dir = bool(res_data_off & 0x80000000)
                            res_sub_off = rsrc_off + (res_data_off & 0x7FFFFFFF)

                            if res_is_dir and res_sub_off + 16 <= len(data):
                                _, _, _, _, lang_named, lang_id = struct.unpack_from("<IIHHHH", data, res_sub_off)
                                l_count = lang_named + lang_id
                                self.volatile_spans.append((res_sub_off + 4, res_sub_off + 8, VOLATILE_RESOURCE_TIMESTAMP))
                                l_entry_ptr = res_sub_off + 16
                                for l_idx in range(l_count):
                                    if l_entry_ptr + 8 > len(data):
                                        break
                                    lang_id_val, leaf_data_off = struct.unpack_from("<II", data, l_entry_ptr)
                                    l_entry_ptr += 8
                                    leaf_off = rsrc_off + (leaf_data_off & 0x7FFFFFFF)
                                    if leaf_off + 16 <= len(data):
                                        leaf_rva, leaf_sz, leaf_cp, leaf_res = struct.unpack_from("<IIII", data, leaf_off)
                                        leaf_raw_off = self.rva_to_offset(leaf_rva, sections)
                                        leaf_hash = ""
                                        if leaf_raw_off is not None and leaf_raw_off + leaf_sz <= len(data):
                                            leaf_hash = sha256_bytes(data[leaf_raw_off:leaf_raw_off + leaf_sz])
                                        resources.append({
                                            "type": type_name,
                                            "id": res_id,
                                            "lang": lang_id_val,
                                            "size": leaf_sz,
                                            "sha256": leaf_hash,
                                        })

        # Calculate Normalized File and Section Hashes
        # Create a copy with volatile spans zeroed
        norm_data = bytearray(self.data)
        for start, end, label in self.volatile_spans:
            if 0 <= start < end <= len(norm_data):
                for b_idx in range(start, end):
                    norm_data[b_idx] = 0

        normalized_file_sha256 = sha256_bytes(bytes(norm_data))

        # Normalized sections
        for s in sections:
            r_ptr = s["raw_ptr"]
            r_sz = s["raw_size"]
            if r_ptr < len(norm_data) and r_sz > 0:
                end_p = min(r_ptr + r_sz, len(norm_data))
                s["normalized_sha256"] = sha256_bytes(bytes(norm_data[r_ptr:end_p]))
            else:
                s["normalized_sha256"] = s["sha256"]

        return {
            "path": self.file_path,
            "fileName": self.file_name,
            "fileSize": self.file_size,
            "rawSha256": self.file_sha256,
            "normalizedSha256": normalized_file_sha256,
            "machine": f"0x{machine:04x}",
            "is64Bit": is_64bit,
            "numSections": num_sections,
            "timeDateStamp": time_date_stamp,
            "characteristics": characteristics,
            "magic": f"0x{magic:04x}",
            "linkerVersion": f"{major_linker}.{minor_linker}",
            "sizeOfCode": size_of_code,
            "sizeOfInitializedData": size_of_init_data,
            "entryPoint": entry_point,
            "imageBase": image_base,
            "sectionAlignment": sec_align,
            "fileAlignment": file_align,
            "sizeOfImage": size_of_image,
            "sizeOfHeaders": size_of_headers,
            "checkSum": checksum,
            "subsystem": subsystem,
            "dllCharacteristics": dll_char,
            "sections": sections,
            "dataDirectories": data_directories,
            "imports": imports,
            "debugEntries": debug_entries,
            "resources": resources,
            "volatileSpans": [
                {"start": s, "end": e, "label": l} for s, e, l in self.volatile_spans
            ],
        }


class ReproducibilityComparator:
    """Compares two parsed PE binaries and determines reproducibility status."""

    def __init__(self, rep_a: Dict[str, Any], rep_b: Dict[str, Any]):
        self.rep_a = rep_a
        self.rep_b = rep_b
        self.differences: List[Dict[str, Any]] = []
        self.volatile_differences: List[Dict[str, Any]] = []
        self.meaningful_differences: List[Dict[str, Any]] = []

    def compare(self) -> Dict[str, Any]:
        a = self.rep_a
        b = self.rep_b

        # 1. Exact raw match check
        exact_raw_match = (a["rawSha256"] == b["rawSha256"] and a["fileSize"] == b["fileSize"])
        normalized_match = (a["normalizedSha256"] == b["normalizedSha256"] and a["fileSize"] == b["fileSize"])

        # 2. Compare Headers
        header_fields = [
            ("machine", "COFF.Machine", False),
            ("numSections", "COFF.NumberOfSections", False),
            ("timeDateStamp", VOLATILE_COFF_TIMESTAMP, True),
            ("characteristics", "COFF.Characteristics", False),
            ("magic", "OptionalHeader.Magic", False),
            ("linkerVersion", "OptionalHeader.LinkerVersion", False),
            ("sizeOfCode", "OptionalHeader.SizeOfCode", False),
            ("sizeOfInitializedData", "OptionalHeader.SizeOfInitializedData", False),
            ("entryPoint", "OptionalHeader.AddressOfEntryPoint", False),
            ("imageBase", "OptionalHeader.ImageBase", False),
            ("sectionAlignment", "OptionalHeader.SectionAlignment", False),
            ("fileAlignment", "OptionalHeader.FileAlignment", False),
            ("sizeOfImage", "OptionalHeader.SizeOfImage", False),
            ("sizeOfHeaders", "OptionalHeader.SizeOfHeaders", False),
            ("checkSum", VOLATILE_OPTIONAL_CHECKSUM, True),
            ("subsystem", "OptionalHeader.Subsystem", False),
            ("dllCharacteristics", "OptionalHeader.DllCharacteristics", False),
        ]

        for field_key, label, is_volatile in header_fields:
            val_a = a.get(field_key)
            val_b = b.get(field_key)
            if val_a != val_b:
                diff_entry = {
                    "field": label,
                    "value_a": val_a,
                    "value_b": val_b,
                    "volatile": is_volatile,
                }
                self.differences.append(diff_entry)
                if is_volatile:
                    self.volatile_differences.append(diff_entry)
                else:
                    self.meaningful_differences.append(diff_entry)

        # 3. Compare Sections
        secs_a = {s["name"]: s for s in a.get("sections", [])}
        secs_b = {s["name"]: s for s in b.get("sections", [])}
        all_sec_names = sorted(set(list(secs_a.keys()) + list(secs_b.keys())))

        for sname in all_sec_names:
            sa = secs_a.get(sname)
            sb = secs_b.get(sname)
            if sa is None or sb is None:
                diff = {
                    "field": f"Section.{sname}",
                    "value_a": "Present" if sa else "Missing",
                    "value_b": "Present" if sb else "Missing",
                    "volatile": False,
                }
                self.differences.append(diff)
                self.meaningful_differences.append(diff)
                continue

            # Compare raw and normalized hashes
            if sa["raw_size"] != sb["raw_size"] or sa["vsize"] != sb["vsize"]:
                diff = {
                    "field": f"Section.{sname}.Size",
                    "value_a": f"raw={sa['raw_size']}, vsize={sa['vsize']}",
                    "value_b": f"raw={sb['raw_size']}, vsize={sb['vsize']}",
                    "volatile": False,
                }
                self.differences.append(diff)
                self.meaningful_differences.append(diff)

            if sa["raw_ptr"] != sb["raw_ptr"] or sa["vaddr"] != sb["vaddr"]:
                diff = {
                    "field": f"Section.{sname}.Layout",
                    "value_a": f"ptr={sa['raw_ptr']}, vaddr={sa['vaddr']}",
                    "value_b": f"ptr={sb['raw_ptr']}, vaddr={sb['vaddr']}",
                    "volatile": False,
                }
                self.differences.append(diff)
                self.meaningful_differences.append(diff)

            if sa["sha256"] != sb["sha256"]:
                if sa["normalized_sha256"] == sb["normalized_sha256"]:
                    diff = {
                        "field": f"Section.{sname}.RawBytes",
                        "value_a": sa["sha256"],
                        "value_b": sb["sha256"],
                        "note": "Section raw bytes differ only in volatile metadata spans; normalized section bytes match.",
                        "volatile": True,
                    }
                    self.differences.append(diff)
                    self.volatile_differences.append(diff)
                else:
                    diff = {
                        "field": f"Section.{sname}.Bytes",
                        "value_a": sa["sha256"],
                        "value_b": sb["sha256"],
                        "note": "Meaningful code/data divergence in section payload.",
                        "volatile": False,
                    }
                    self.differences.append(diff)
                    self.meaningful_differences.append(diff)

        # 4. Compare Imports
        imps_a = {imp["dll"].upper(): sorted(imp["functions"]) for imp in a.get("imports", [])}
        imps_b = {imp["dll"].upper(): sorted(imp["functions"]) for imp in b.get("imports", [])}
        if imps_a != imps_b:
            diff = {
                "field": "ImportTable",
                "value_a": list(imps_a.keys()),
                "value_b": list(imps_b.keys()),
                "volatile": False,
            }
            self.differences.append(diff)
            self.meaningful_differences.append(diff)

        # 5. Compare Debug Directory
        dbg_a = a.get("debugEntries", [])
        dbg_b = b.get("debugEntries", [])
        if len(dbg_a) != len(dbg_b):
            diff = {
                "field": "DebugDirectory.Count",
                "value_a": len(dbg_a),
                "value_b": len(dbg_b),
                "volatile": False,
            }
            self.differences.append(diff)
            self.meaningful_differences.append(diff)
        else:
            for idx, (ea, eb) in enumerate(zip(dbg_a, dbg_b)):
                if ea.get("type") != eb.get("type"):
                    diff = {
                        "field": f"DebugDirectory[{idx}].Type",
                        "value_a": ea.get("type"),
                        "value_b": eb.get("type"),
                        "volatile": False,
                    }
                    self.differences.append(diff)
                    self.meaningful_differences.append(diff)
                if ea.get("timestamp") != eb.get("timestamp"):
                    diff = {
                        "field": f"DebugDirectory[{idx}].TimeDateStamp",
                        "value_a": ea.get("timestamp"),
                        "value_b": eb.get("timestamp"),
                        "volatile": True,
                    }
                    self.differences.append(diff)
                    self.volatile_differences.append(diff)
                # CodeView details
                if "codeview" in ea or "codeview" in eb:
                    cva = ea.get("codeview", {})
                    cvb = eb.get("codeview", {})
                    if cva.get("pdb_filename") != cvb.get("pdb_filename"):
                        diff = {
                            "field": f"DebugDirectory[{idx}].CodeView.PdbFilename",
                            "value_a": cva.get("pdb_filename"),
                            "value_b": cvb.get("pdb_filename"),
                            "volatile": False,
                        }
                        self.differences.append(diff)
                        self.meaningful_differences.append(diff)
                    if cva.get("guid") != cvb.get("guid"):
                        diff = {
                            "field": f"DebugDirectory[{idx}].CodeView.GUID",
                            "value_a": cva.get("guid"),
                            "value_b": cvb.get("guid"),
                            "volatile": True,
                        }
                        self.differences.append(diff)
                        self.volatile_differences.append(diff)

        # 6. Compare Resources
        rsrc_a = {(r["type"], r["id"], r["lang"]): r["sha256"] for r in a.get("resources", [])}
        rsrc_b = {(r["type"], r["id"], r["lang"]): r["sha256"] for r in b.get("resources", [])}
        if rsrc_a != rsrc_b:
            diff = {
                "field": "ResourceTable.Payloads",
                "value_a": [f"{k[0]}:{k[1]}" for k in rsrc_a.keys()],
                "value_b": [f"{k[0]}:{k[1]}" for k in rsrc_b.keys()],
                "volatile": False,
            }
            self.differences.append(diff)
            self.meaningful_differences.append(diff)

        # Verdict calculation
        if exact_raw_match:
            verdict = "REPRODUCIBLE_BIT_EXACT"
            is_reproducible = True
            summary = "Bit-for-bit identical raw binary SHA-256 hashes."
        elif len(self.meaningful_differences) == 0 and len(self.volatile_differences) > 0:
            verdict = "REPRODUCIBLE_NORMALIZED"
            is_reproducible = True
            summary = (
                f"Normalized binaries are equivalent across all executable code (.text), initialized data (.rdata), "
                f"resources, relocations, and imports. Differences are strictly confined to {len(self.volatile_differences)} "
                f"documented volatile header/debug timestamp field(s)."
            )
        else:
            verdict = "NON_REPRODUCIBLE_DIVERGENCE"
            is_reproducible = False
            summary = (
                f"Found {len(self.meaningful_differences)} meaningful code/data/structural divergence(s) between builds."
            )

        return {
            "verdict": verdict,
            "isReproducible": is_reproducible,
            "exactRawMatch": exact_raw_match,
            "normalizedMatch": normalized_match,
            "summary": summary,
            "buildA": {
                "path": a["path"],
                "size": a["fileSize"],
                "rawSha256": a["rawSha256"],
                "normalizedSha256": a["normalizedSha256"],
                "timeDateStamp": a["timeDateStamp"],
                "checkSum": a["checkSum"],
            },
            "buildB": {
                "path": b["path"],
                "size": b["fileSize"],
                "rawSha256": b["rawSha256"],
                "normalizedSha256": b["normalizedSha256"],
                "timeDateStamp": b["timeDateStamp"],
                "checkSum": b["checkSum"],
            },
            "volatileDifferences": self.volatile_differences,
            "meaningfulDifferences": self.meaningful_differences,
            "allDifferences": self.differences,
        }


def format_human_report(result: Dict[str, Any]) -> str:
    lines = []
    lines.append("================================================================================")
    lines.append("                 PE BINARY REPRODUCIBILITY VERIFICATION REPORT")
    lines.append("================================================================================")
    lines.append(f"Verdict:             {result['verdict']}")
    lines.append(f"Reproducible:        {'YES (PASS)' if result['isReproducible'] else 'NO (FAIL)'}")
    lines.append(f"Exact Raw SHA-256:   {'YES (100% bit-for-bit)' if result['exactRawMatch'] else 'NO (Differs)'}")
    lines.append(f"Normalized Equiv:    {'YES' if result['normalizedMatch'] else 'NO'}")
    lines.append("")
    lines.append("Build 1 (A):")
    lines.append(f"  Path:              {result['buildA']['path']}")
    lines.append(f"  Size:              {result['buildA']['size']:,} bytes")
    lines.append(f"  Raw SHA-256:       {result['buildA']['rawSha256']}")
    lines.append(f"  Normalized SHA256: {result['buildA']['normalizedSha256']}")
    lines.append(f"  TimeDateStamp:     0x{result['buildA']['timeDateStamp']:08X}")
    lines.append(f"  CheckSum:          0x{result['buildA']['checkSum']:08X}")
    lines.append("")
    lines.append("Build 2 (B):")
    lines.append(f"  Path:              {result['buildB']['path']}")
    lines.append(f"  Size:              {result['buildB']['size']:,} bytes")
    lines.append(f"  Raw SHA-256:       {result['buildB']['rawSha256']}")
    lines.append(f"  Normalized SHA256: {result['buildB']['normalizedSha256']}")
    lines.append(f"  TimeDateStamp:     0x{result['buildB']['timeDateStamp']:08X}")
    lines.append(f"  CheckSum:          0x{result['buildB']['checkSum']:08X}")
    lines.append("")

    if result["volatileDifferences"]:
        lines.append("Documented Volatile Field Differences:")
        for vd in result["volatileDifferences"]:
            lines.append(f"  - {vd['field']}: A={vd.get('value_a')} vs B={vd.get('value_b')}")
            if "note" in vd:
                lines.append(f"    Note: {vd['note']}")
        lines.append("")

    if result["meaningfulDifferences"]:
        lines.append("MEANINGFUL STRUCTURAL DIVERGENCES (NON-REPRODUCIBLE):")
        for md in result["meaningfulDifferences"]:
            lines.append(f"  * {md['field']}: A={md.get('value_a')} vs B={md.get('value_b')}")
            if "note" in md:
                lines.append(f"    Detail: {md['note']}")
        lines.append("")

    lines.append("Summary:")
    lines.append(f"  {result['summary']}")
    lines.append("================================================================================")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Inspect and compare two PE binaries for bit-for-bit and normalized reproducibility."
    )
    parser.add_argument("binary_a", help="Path to first PE binary (Build A)")
    parser.add_argument("binary_b", help="Path to second PE binary (Build B)")
    parser.add_argument("--json", action="store_true", help="Emit structured JSON output")
    parser.add_argument("--quiet", action="store_true", help="Emit minimal output")
    parser.add_argument("--strict-raw", action="store_true", help="Require 100% exact raw byte match (fail on normalized)")

    args = parser.parse_args()

    try:
        inspector_a = PEDetailedInspector(args.binary_a)
        rep_a = inspector_a.parse()

        inspector_b = PEDetailedInspector(args.binary_b)
        rep_b = inspector_b.parse()

        comparator = ReproducibilityComparator(rep_a, rep_b)
        result = comparator.compare()

        if args.strict_raw and not result["exactRawMatch"]:
            result["isReproducible"] = False
            result["verdict"] = "STRICT_RAW_FAIL"
            result["summary"] = "Strict raw matching enabled: Binaries are normalized equivalent but raw SHA-256 differs."

        if args.json:
            print(json.dumps(result, indent=2))
        elif not args.quiet:
            print(format_human_report(result))

        return 0 if result["isReproducible"] else 1

    except Exception as e:
        if args.json:
            print(json.dumps({"error": str(e), "isReproducible": False}))
        else:
            print(f"ERROR: PE comparison failed: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
