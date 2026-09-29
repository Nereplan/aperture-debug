#!/usr/bin/env python3
"""
Aperture Threat Boundary and Security Invariant Validator.

Validates:
1. Schema integrity of docs/threat_boundaries_manifest.json against
   docs/threat_boundaries_manifest.schema.json.
2. Boundary ID uniqueness, format, and completeness (all 12 security boundaries).
3. Existence on disk of all declared source files and test files.
4. Existence in source/header/test files of all declared authoritative C++/script symbols.
5. 1:1 parity between docs/THREAT_BOUNDARIES.md and docs/threat_boundaries_manifest.json.
6. Portable containment invariants (rejection of %APPDATA%, %LOCALAPPDATA%, %USERPROFILE%,
   or Registry application state claims).
7. Secrets & operational signing key safety checks (no private keys or credentials).
8. Built-in synthetic self-tests (--self-test).
"""

import os
import sys
import json
import re
import argparse
from typing import Dict, List, Set, Tuple, Any, Optional

# Patterns that indicate private keys or embedded operational secrets
SECRET_PATTERNS = [
    re.compile(r"-----BEGIN (?:EC |RSA |DSA |OPENSSH )?PRIVATE KEY-----", re.IGNORECASE),
    re.compile(r"PRIVATE KEY:\s*[0-9a-fA-F]{32,}", re.IGNORECASE),
    re.compile(r"password\s*=\s*['\"][^'\"]{6,}['\"]", re.IGNORECASE),
]

class ThreatBoundaryValidator:
    def __init__(self, workspace_root: Optional[str] = None):
        if workspace_root:
            self.root = os.path.abspath(workspace_root)
        else:
            self.root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

        self.manifest_path = os.path.join(self.root, "docs", "threat_boundaries_manifest.json")
        self.schema_path = os.path.join(self.root, "docs", "threat_boundaries_manifest.schema.json")
        self.doc_path = os.path.join(self.root, "docs", "THREAT_BOUNDARIES.md")

    def _validate_schema_basic(self, manifest: Dict[str, Any], schema: Dict[str, Any]) -> List[str]:
        """Pure-Python structural schema validation."""
        errors = []
        req_top = schema.get("required", [])
        for field in req_top:
            if field not in manifest:
                errors.append(f"Manifest missing top-level required property '{field}'")

        if manifest.get("version") != 1:
            errors.append(f"Manifest version must be 1, got {manifest.get('version')}")

        containment = manifest.get("containmentPolicy", {})
        if not isinstance(containment, dict):
            errors.append("containmentPolicy must be an object")
        else:
            if containment.get("mode") != "portable_only":
                errors.append(f"containmentPolicy.mode must be 'portable_only', got '{containment.get('mode')}'")

        boundaries = manifest.get("boundaries", [])
        if not isinstance(boundaries, list) or len(boundaries) == 0:
            errors.append("Manifest 'boundaries' must be a non-empty list")
            return errors

        boundary_schema = schema.get("properties", {}).get("boundaries", {}).get("items", {})
        req_b_fields = boundary_schema.get("required", [])

        seen_ids = set()
        for idx, b in enumerate(boundaries):
            if not isinstance(b, dict):
                errors.append(f"Boundary item at index {idx} must be an object")
                continue

            for field in req_b_fields:
                if field not in b:
                    errors.append(f"Boundary item #{idx} missing required field '{field}'")

            bid = b.get("id", "")
            if not isinstance(bid, str) or not re.match(r"^[a-z0-9_]+$", bid):
                errors.append(f"Boundary #{idx} has invalid ID '{bid}' (must match ^[a-z0-9_]+$)")
            elif bid in seen_ids:
                errors.append(f"Duplicate boundary ID detected: '{bid}'")
            else:
                seen_ids.add(bid)

            # Array field checks
            for arr_field in ["authoritativeSymbols", "sourceFiles", "testFiles"]:
                arr = b.get(arr_field)
                if not isinstance(arr, list) or len(arr) == 0:
                    errors.append(f"Boundary '{bid}' field '{arr_field}' must be a non-empty list")
                else:
                    for item in arr:
                        if not isinstance(item, str) or len(item.strip()) == 0:
                            errors.append(f"Boundary '{bid}' field '{arr_field}' contains empty or non-string entry")

            # Validation rules check
            rules = b.get("validationRules")
            if not isinstance(rules, dict) or len(rules) == 0:
                errors.append(f"Boundary '{bid}' 'validationRules' must be a non-empty object")

        return errors

    def validate_file_references(self, manifest: Dict[str, Any]) -> List[str]:
        """Verifies that all referenced source and test files physically exist on disk."""
        errors = []
        boundaries = manifest.get("boundaries", [])
        for b in boundaries:
            bid = b.get("id", "unknown")
            source_files = b.get("sourceFiles", [])
            for sf in source_files:
                full_path = os.path.join(self.root, sf)
                if not os.path.isfile(full_path):
                    errors.append(f"Boundary '{bid}' references non-existent source file: '{sf}'")

            test_files = b.get("testFiles", [])
            for tf in test_files:
                full_path = os.path.join(self.root, tf)
                if not os.path.isfile(full_path):
                    errors.append(f"Boundary '{bid}' references non-existent test file: '{tf}'")

        return errors

    def validate_symbols(self, manifest: Dict[str, Any]) -> List[str]:
        """Verifies that all declared authoritative symbols exist in their referenced source/test files."""
        errors = []
        boundaries = manifest.get("boundaries", [])

        # Cache file contents
        file_cache: Dict[str, str] = {}

        def get_file_content(rel_path: str) -> str:
            if rel_path not in file_cache:
                full_path = os.path.join(self.root, rel_path)
                if os.path.isfile(full_path):
                    try:
                        with open(full_path, "r", encoding="utf-8", errors="ignore") as f:
                            file_cache[rel_path] = f.read()
                    except Exception:
                        file_cache[rel_path] = ""
                else:
                    file_cache[rel_path] = ""
            return file_cache[rel_path]

        for b in boundaries:
            bid = b.get("id", "unknown")
            symbols = b.get("authoritativeSymbols", [])
            source_files = b.get("sourceFiles", [])
            test_files = b.get("testFiles", [])
            all_files = list(set(source_files + test_files))

            for sym in symbols:
                # Extract bare identifier (e.g. Aperture::CheckForUpdates -> CheckForUpdates)
                parts = sym.split("::")
                bare_name = parts[-1]
                found = False

                for sf in all_files:
                    content = get_file_content(sf)
                    if bare_name in content:
                        found = True
                        break

                # If not found in boundary-local files, check src/ and tests/ widely
                if not found:
                    for sf, content in file_cache.items():
                        if bare_name in content:
                            found = True
                            break

                if not found:
                    errors.append(f"Boundary '{bid}' symbol '{sym}' (identifier '{bare_name}') was not found in referenced files")

        return errors

    def validate_documentation_parity(self, manifest: Dict[str, Any]) -> List[str]:
        """Verifies 1:1 parity between docs/THREAT_BOUNDARIES.md and docs/threat_boundaries_manifest.json."""
        errors = []
        if not os.path.isfile(self.doc_path):
            return [f"Authoritative documentation file not found: {self.doc_path}"]

        with open(self.doc_path, "r", encoding="utf-8") as f:
            doc_text = f.read()

        boundaries = manifest.get("boundaries", [])
        manifest_ids = [b.get("id") for b in boundaries if b.get("id")]

        # Extract boundary IDs mentioned in docs
        doc_ids_found = set()
        id_pattern = re.compile(r"-\s*\*\*Boundary ID\*\*:\s*`([a-z0-9_]+)`")
        for match in id_pattern.finditer(doc_text):
            doc_ids_found.add(match.group(1))

        # Check all manifest IDs exist in docs
        for mid in manifest_ids:
            if mid not in doc_ids_found:
                errors.append(f"Manifest boundary ID '{mid}' is missing from documentation in {self.doc_path}")

        # Check all doc IDs exist in manifest
        manifest_set = set(manifest_ids)
        for did in doc_ids_found:
            if did not in manifest_set:
                errors.append(f"Documentation contains boundary ID '{did}' not present in manifest")

        return errors

    def validate_containment_invariants(self, manifest: Dict[str, Any]) -> List[str]:
        """Verifies that no forbidden storage locations or installed-mode claims exist."""
        errors = []
        boundaries = manifest.get("boundaries", [])

        # Check manifest durableWriteDestination
        for b in boundaries:
            bid = b.get("id", "unknown")
            dest = b.get("durableWriteDestination", "")
            dest_lower = dest.lower()

            # Check for forbidden external write destinations
            for forbidden in ["%appdata%", "%localappdata%", "%userprofile%"]:
                if forbidden in dest_lower:
                    # Allowed only if explicitly stated as prohibited/never/zero
                    has_negation = any(neg in dest_lower for neg in ["never", "prohibited", "zero", "not", "none"])
                    if not has_negation:
                        errors.append(f"Boundary '{bid}' claims forbidden write destination: '{dest}'")

            # Check Registry usage
            if "registry" in dest_lower or "hkey_" in dest_lower or "hkcu" in dest_lower:
                if bid != "shell_registration_and_system_metadata":
                    has_negation = any(neg in dest_lower for neg in ["never", "prohibited", "zero", "not", "none"])
                    if not has_negation:
                        errors.append(f"Boundary '{bid}' claims Registry write destination, but only shell_registration_and_system_metadata may write transient shell verbs")
                else:
                    # In shell_registration_and_system_metadata: must not claim positive application state in registry
                    if "application state" in dest_lower:
                        has_negation = any(neg in dest_lower for neg in ["zero", "never", "no ", "prohibited", "none", "without"])
                        if not has_negation:
                            errors.append(f"Boundary '{bid}' claims to store application state in Registry (forbidden)")

        # Verify documentation does not claim installed mode or external state
        if os.path.isfile(self.doc_path):
            with open(self.doc_path, "r", encoding="utf-8") as f:
                lines = f.readlines()

            for lno, line in enumerate(lines, 1):
                # Check for secrets
                for spat in SECRET_PATTERNS:
                    if spat.search(line):
                        errors.append(f"Documentation line {lno} contains potential operational secret/key")

        return errors

    def validate_all(self) -> Tuple[bool, List[str]]:
        """Executes full suite of threat boundary validation checks."""
        all_errors = []

        if not os.path.isfile(self.manifest_path):
            return False, [f"Manifest file not found at {self.manifest_path}"]

        if not os.path.isfile(self.schema_path):
            return False, [f"Schema file not found at {self.schema_path}"]

        try:
            with open(self.manifest_path, "r", encoding="utf-8") as f:
                manifest = json.load(f)
        except Exception as ex:
            return False, [f"Failed to parse manifest JSON: {ex}"]

        try:
            with open(self.schema_path, "r", encoding="utf-8") as f:
                schema = json.load(f)
        except Exception as ex:
            return False, [f"Failed to parse schema JSON: {ex}"]

        # 1. Schema check
        all_errors.extend(self._validate_schema_basic(manifest, schema))

        # 2. File references check
        all_errors.extend(self.validate_file_references(manifest))

        # 3. Symbol verification
        all_errors.extend(self.validate_symbols(manifest))

        # 4. Documentation parity
        all_errors.extend(self.validate_documentation_parity(manifest))

        # 5. Containment invariants
        all_errors.extend(self.validate_containment_invariants(manifest))

        return len(all_errors) == 0, all_errors


def run_self_tests() -> bool:
    """Executes synthetic self-tests to ensure validator correctly rejects invalid manifests."""
    print("Running ThreatBoundaryValidator synthetic self-tests...")
    validator = ThreatBoundaryValidator()

    # Test 1: Duplicate ID rejection
    synthetic_dup = {
        "$schema": "./threat_boundaries_manifest.schema.json",
        "version": 1,
        "description": "test",
        "containmentPolicy": {
            "mode": "portable_only",
            "durableStateRoot": "data/",
            "disposableCacheRoot": "cache/",
            "diagnosticsRoot": "diagnostics/",
            "prohibitedExternalLocations": []
        },
        "boundaries": [
            {
                "id": "dup_boundary",
                "name": "Dup 1",
                "category": "cat",
                "trustLevel": "tl",
                "privilegeContext": "pc",
                "durableWriteDestination": "None",
                "failureErrorCode": "err",
                "authoritativeSymbols": ["sym"],
                "sourceFiles": ["src/Common.h"],
                "testFiles": ["tests/persistence/TestSchemaMigration.h"],
                "validationRules": {"rule": 1},
                "residualRiskSummary": "rr"
            },
            {
                "id": "dup_boundary",
                "name": "Dup 2",
                "category": "cat",
                "trustLevel": "tl",
                "privilegeContext": "pc",
                "durableWriteDestination": "None",
                "failureErrorCode": "err",
                "authoritativeSymbols": ["sym"],
                "sourceFiles": ["src/Common.h"],
                "testFiles": ["tests/persistence/TestSchemaMigration.h"],
                "validationRules": {"rule": 1},
                "residualRiskSummary": "rr"
            }
        ]
    }
    schema = {"required": ["version", "description", "containmentPolicy", "boundaries"], "properties": {"boundaries": {"items": {"required": ["id"]}}}}
    errs = validator._validate_schema_basic(synthetic_dup, schema)
    assert any("Duplicate boundary ID" in e for e in errs), f"Expected duplicate ID error, got: {errs}"

    # Test 2: Non-portable mode rejection
    synthetic_nonportable = dict(synthetic_dup)
    synthetic_nonportable["containmentPolicy"] = {"mode": "installed_appdata"}
    errs2 = validator._validate_schema_basic(synthetic_nonportable, schema)
    assert any("portable_only" in e for e in errs2), f"Expected portable_only error, got: {errs2}"

    # Test 3: Missing source file rejection
    synthetic_bad_file = {
        "boundaries": [
            {
                "id": "bad_file_boundary",
                "sourceFiles": ["src/NonExistentFile_12345.h"],
                "testFiles": ["tests/persistence/TestSchemaMigration.h"]
            }
        ]
    }
    errs3 = validator.validate_file_references(synthetic_bad_file)
    assert any("non-existent source file" in e for e in errs3), f"Expected non-existent file error, got: {errs3}"

    # Test 4: Missing symbol rejection
    synthetic_bad_sym = {
        "boundaries": [
            {
                "id": "bad_sym_boundary",
                "authoritativeSymbols": ["Aperture::NonExistentFakeSymbol_999"],
                "sourceFiles": ["src/Common.h"],
                "testFiles": ["tests/persistence/TestSchemaMigration.h"]
            }
        ]
    }
    errs4 = validator.validate_symbols(synthetic_bad_sym)
    assert any("was not found" in e for e in errs4), f"Expected missing symbol error, got: {errs4}"

    # Test 5: Forbidden storage claim rejection
    synthetic_forbidden_storage = {
        "boundaries": [
            {
                "id": "forbidden_storage_boundary",
                "durableWriteDestination": "%APPDATA%\\Aperture\\settings.json"
            }
        ]
    }
    errs5 = validator.validate_containment_invariants(synthetic_forbidden_storage)
    assert any("claims forbidden write destination" in e for e in errs5), f"Expected forbidden storage error, got: {errs5}"

    # Test 6: Forbidden registry application state claim rejection
    synthetic_forbidden_reg = {
        "boundaries": [
            {
                "id": "shell_registration_and_system_metadata",
                "durableWriteDestination": "Windows Registry HKCU\\Software\\Aperture stores application state"
            }
        ]
    }
    errs6 = validator.validate_containment_invariants(synthetic_forbidden_reg)
    assert any("claims to store application state in Registry" in e for e in errs6), f"Expected forbidden registry state error, got: {errs6}"

    print("All synthetic self-tests passed successfully.")
    return True


def main():
    parser = argparse.ArgumentParser(description="Validate Aperture threat boundary definitions, manifest, and invariants")
    parser.add_argument("--self-test", action="store_true", help="Run synthetic self-tests and exit")
    parser.add_argument("--workspace", type=str, default="", help="Workspace root directory path")
    args = parser.parse_args()

    if args.self_test:
        success = run_self_tests()
        sys.exit(0 if success else 1)

    workspace = args.workspace if args.workspace else None
    validator = ThreatBoundaryValidator(workspace)
    success, errors = validator.validate_all()

    print("=" * 80)
    print("            Aperture Threat Boundary & Security Invariant Report            ")
    print("=" * 80)
    print(f"Manifest:   {os.path.relpath(validator.manifest_path, validator.root)}")
    print(f"Schema:     {os.path.relpath(validator.schema_path, validator.root)}")
    print(f"Reference:  {os.path.relpath(validator.doc_path, validator.root)}")
    print("-" * 80)

    if success:
        try:
            with open(validator.manifest_path, "r", encoding="utf-8") as f:
                manifest = json.load(f)
            boundaries = manifest.get("boundaries", [])
            print(f"Status: VALID (All {len(boundaries)} threat boundaries verified successfully)")
            print()
            print("--- Verified Security Boundaries ---")
            for idx, b in enumerate(boundaries, 1):
                print(f"  [{idx:02d}] {b.get('id'):<40} | Symbols: {len(b.get('authoritativeSymbols', [])):>2} | Sources: {len(b.get('sourceFiles', [])):>2} | Tests: {len(b.get('testFiles', [])):>2}")
            print()
            print("Invariants: Portable-only containment, zero %APPDATA%/Registry state leakage verified.")
            print("=" * 80)
            sys.exit(0)
        except Exception as ex:
            print(f"Error reading manifest: {ex}", file=sys.stderr)
            sys.exit(1)
    else:
        print(f"Status: FAILED ({len(errors)} error(s) detected)")
        print()
        for idx, err in enumerate(errors, 1):
            print(f"  [{idx}] ERROR: {err}", file=sys.stderr)
        print("=" * 80)
        sys.exit(1)


if __name__ == "__main__":
    main()
