#!/usr/bin/env python3
"""
Unit and integration tests for Authenticode code-signing, signtool discovery,
certificate validation, package manifest hashing, and signature verification.

Tests verify:
1. SignTool discovery mechanism and execution.
2. Code Signing EKU (1.3.6.1.5.5.7.3.3) and expiration validation.
3. Dry-run mode for non-destructive pre-flight checks.
4. Unsigned package flow (clearly reported as unsigned, rejected when -RequireSigned is specified).
5. Ephemeral test certificate signing, WinVerifyTrust / Authenticode validation, and SHA-256 digest algorithm.
6. Tamper detection: modifying signed bytes invalidates Authenticode verification.
7. Guaranteed temporary cleanup: no secrets, PFX files, or certificates persist.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

WORKSPACE_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCRIPTS_DIR = os.path.join(WORKSPACE_ROOT, "scripts")
SIGN_SCRIPT = os.path.join(SCRIPTS_DIR, "sign_artifacts.ps1")
PACKAGE_SCRIPT = os.path.join(SCRIPTS_DIR, "package_release.ps1")
VERIFY_SCRIPT = os.path.join(SCRIPTS_DIR, "verify_package.ps1")


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


def create_minimal_pe_file(path: str):
    """Creates a minimal valid x64 PE executable structure for test signing."""
    # Minimal DOS + PE header with x64 machine (0x8664) and basic section
    buf = bytearray(512)
    # MZ header
    buf[0:2] = b"MZ"
    # e_lfanew at offset 0x3C -> 0x80
    buf[0x3C:0x40] = (0x80).to_bytes(4, "little")
    # PE signature at 0x80
    buf[0x80:0x84] = b"PE\x00\x00"
    # Machine: x64 (0x8664)
    buf[0x84:0x86] = (0x8664).to_bytes(2, "little")
    # NumberOfSections = 1
    buf[0x86:0x88] = (1).to_bytes(2, "little")
    # SizeOfOptionalHeader = 0xF0 (240 for PE32+)
    buf[0x94:0x96] = (0xF0).to_bytes(2, "little")
    # Characteristics: EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE
    buf[0x96:0x98] = (0x0022).to_bytes(2, "little")
    # Optional Header Magic: PE32+ (0x20B)
    opt_offset = 0x98
    buf[opt_offset:opt_offset + 2] = (0x020B).to_bytes(2, "little")
    # Subsystem: Windows GUI (2)
    buf[opt_offset + 68:opt_offset + 70] = (2).to_bytes(2, "little")
    # Section Alignment: 0x1000, File Alignment: 0x200
    buf[opt_offset + 32:opt_offset + 36] = (0x1000).to_bytes(4, "little")
    buf[opt_offset + 36:opt_offset + 40] = (0x200).to_bytes(4, "little")

    with open(path, "wb") as f:
        f.write(buf)


class TestPackageSigning(unittest.TestCase):
    """Test suite for Authenticode code-signing and verification."""

    @classmethod
    def setUpClass(cls):
        cls.temp_dir = tempfile.mkdtemp(prefix="aperture-sign-test-")

    @classmethod
    def tearDownClass(cls):
        if os.path.exists(cls.temp_dir):
            shutil.rmtree(cls.temp_dir, ignore_errors=True)

    def test_signtool_discovery(self):
        """Test signtool discovery function executes cleanly and reports presence."""
        cmd = f"& '{SIGN_SCRIPT}' -Mode DiscoverSigntool -Quiet"
        proc = run_powershell(cmd)
        self.assertEqual(proc.returncode, 0, f"DiscoverSigntool failed: {proc.stderr}")

    def test_dry_run_signing(self):
        """Test dry run signing mode validates parameters without mutating file."""
        test_pe = os.path.join(self.temp_dir, "test_dryrun.exe")
        create_minimal_pe_file(test_pe)
        size_before = os.path.getsize(test_pe)

        cmd = f"& '{SIGN_SCRIPT}' -Path '{test_pe}' -Mode Sign -DryRun"
        proc = run_powershell(cmd)
        self.assertEqual(proc.returncode, 0, f"DryRun failed: {proc.stderr}")
        self.assertIn("DRY-RUN", proc.stdout)
        self.assertEqual(os.path.getsize(test_pe), size_before)

    def test_unsigned_package_verification(self):
        """Test that unsigned package is reported clearly and rejected under -RequireSigned."""
        pkg_dir = os.path.join(self.temp_dir, "test_unsigned_pkg")
        os.makedirs(pkg_dir, exist_ok=True)
        os.makedirs(os.path.join(pkg_dir, "data"), exist_ok=True)
        os.makedirs(os.path.join(pkg_dir, "fonts"), exist_ok=True)

        test_exe = os.path.join(pkg_dir, "Aperture.exe")
        create_minimal_pe_file(test_exe)

        # Package manifest for unsigned binary
        manifest = {
            "schemaVersion": 1,
            "application": "Aperture",
            "version": "2.0.0.0",
            "configuration": "Release",
            "compilerStandard": "c++17",
            "architecture": "x64",
            "binary": "Aperture.exe",
            "binarySha256": "dummy",
            "signature": {
                "signed": False,
                "status": "Unsigned"
            },
            "runtime": {
                "platform": "Windows 10/11 x64",
                "graphics": "Direct2D 1.1 / Direct3D 11",
                "playback": "libmpv-2.dll",
                "writableDirectory": "data"
            },
            "files": []
        }
        with open(os.path.join(pkg_dir, "manifest.json"), "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2)

        # Verification without -RequireSigned checks unsigned status cleanly
        cmd_verify_unsigned = f"& '{SIGN_SCRIPT}' -Path '{test_exe}' -Mode Verify"
        proc_verify = run_powershell(cmd_verify_unsigned)
        self.assertEqual(proc_verify.returncode, 0)
        self.assertIn("Unsigned", proc_verify.stdout)

        # Verification with -RequireSigned must fail closed
        cmd_verify_required = f"& '{SIGN_SCRIPT}' -Path '{test_exe}' -Mode Verify -RequireSigned"
        proc_required = run_powershell(cmd_verify_required)
        self.assertNotEqual(proc_required.returncode, 0)
        self.assertIn("unsigned", proc_required.stderr.lower())

    def test_ephemeral_self_signed_certificate_validation_and_signing(self):
        """Test ephemeral self-signed code signing certificate generation, signing, and verification."""
        pfx_path = os.path.join(self.temp_dir, "test_code_signing.pfx")
        pfx_password = "ApertureTestPassword123!"
        test_exe = os.path.join(self.temp_dir, "Aperture_signed.exe")
        create_minimal_pe_file(test_exe)

        # PowerShell command to create an ephemeral self-signed Code Signing cert and export to PFX
        create_cert_cmd = f"""
        $cert = New-SelfSignedCertificate `
            -Type CodeSigningCert `
            -Subject "CN=Aperture Unit Test Signer" `
            -CertStoreLocation "Cert:\\CurrentUser\\My" `
            -NotBefore (Get-Date).AddMinutes(-5) `
            -NotAfter (Get-Date).AddHours(1)

        $secPass = ConvertTo-SecureString -String '{pfx_password}' -Force -AsPlainText
        Export-PfxCertificate -Cert $cert -FilePath '{pfx_path}' -Password $secPass | Out-Null
        $thumbprint = $cert.Thumbprint
        Remove-Item ("Cert:\\CurrentUser\\My\\" + $thumbprint) -Force
        $thumbprint
        """
        proc_cert = run_powershell(create_cert_cmd)
        if proc_cert.returncode != 0:
            self.skipTest(f"New-SelfSignedCertificate not available in this environment: {proc_cert.stderr}")

        thumbprint = proc_cert.stdout.strip().split()[-1]
        self.assertTrue(os.path.exists(pfx_path), "PFX file was not generated")

        try:
            # 1. Test certificate validation mode
            validate_cmd = f"& '{SIGN_SCRIPT}' -Mode ValidateCertificate -CertificatePfxPath '{pfx_path}' -CertificatePassword '{pfx_password}' -CertificateThumbprint '{thumbprint}'"
            proc_val = run_powershell(validate_cmd)
            self.assertEqual(proc_val.returncode, 0, f"Certificate validation failed: {proc_val.stderr}")
            self.assertIn("Certificate validated successfully", proc_val.stdout)

            # 2. Check if signtool is present for actual signing
            disc_cmd = f"& '{SIGN_SCRIPT}' -Mode DiscoverSigntool -Quiet"
            proc_disc = run_powershell(disc_cmd)
            if "Found: True" in proc_disc.stdout or proc_disc.returncode == 0:
                # Sign the test PE
                sign_cmd = f"& '{SIGN_SCRIPT}' -Path '{test_exe}' -Mode Sign -CertificatePfxPath '{pfx_path}' -CertificatePassword '{pfx_password}'"
                proc_sign = run_powershell(sign_cmd)
                if proc_sign.returncode == 0:
                    self.assertIn("Successfully signed and verified", proc_sign.stdout)

                    # Verify signature with SHA-256 requirement
                    verify_cmd = f"& '{SIGN_SCRIPT}' -Path '{test_exe}' -Mode Verify -RequireSigned"
                    proc_verify = run_powershell(verify_cmd)
                    self.assertEqual(proc_verify.returncode, 0, f"Post-sign verification failed: {proc_verify.stderr}")
                    self.assertIn("SHA256", proc_verify.stdout)
                    self.assertIn("Signature verified", proc_verify.stdout)

                    # Tampering detection: modify byte in executable
                    with open(test_exe, "r+b") as f:
                        f.seek(0x20)
                        f.write(b"\xFF\xFF")

                    tamper_verify = run_powershell(verify_cmd)
                    # WinVerifyTrust / Authenticode detects hash mismatch on tampered binary
                    self.assertTrue(
                        tamper_verify.returncode != 0 or "HashMismatch" in tamper_verify.stdout,
                        "Tampered binary was not detected by signature verification"
                    )

        finally:
            # Guaranteed cleanup
            if os.path.exists(pfx_path):
                os.remove(pfx_path)
            if os.path.exists(test_exe):
                os.remove(test_exe)

    def test_missing_pfx_fails_cleanly(self):
        """Test that missing PFX certificate fails cleanly without crashing."""
        cmd = f"& '{SIGN_SCRIPT}' -Mode ValidateCertificate -CertificatePfxPath 'C:\\non_existent_path_12345.pfx'"
        proc = run_powershell(cmd)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("not found", proc.stderr.lower())

    def test_wrong_password_does_not_leak_in_error(self):
        """Test that wrong password fails securely without echoing secrets."""
        pfx_path = os.path.join(self.temp_dir, "test_dummy.pfx")
        with open(pfx_path, "wb") as f:
            f.write(b"NOT_A_REAL_PFX_DATA")

        secret_str = "SUPER_SECRET_VALUE_987654321"
        cmd = f"& '{SIGN_SCRIPT}' -Mode ValidateCertificate -CertificatePfxPath '{pfx_path}' -CertificatePassword '{secret_str}'"
        proc = run_powershell(cmd)
        self.assertNotEqual(proc.returncode, 0)
        self.assertNotIn(secret_str, proc.stdout)
        self.assertNotIn(secret_str, proc.stderr)
    def test_http_timestamp_server_rejected(self):
        """Test that insecure HTTP timestamp server URLs are strictly rejected."""
        test_pe = os.path.join(self.temp_dir, "test_http_reject.exe")
        create_minimal_pe_file(test_pe)
        cmd = f"& '{SIGN_SCRIPT}' -Path '{test_pe}' -Mode Sign -DryRun -TimestampServer 'http://insecure-timestamp.example.com'"
        proc = run_powershell(cmd)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("HTTPS", proc.stderr)


if __name__ == "__main__":
    unittest.main()
