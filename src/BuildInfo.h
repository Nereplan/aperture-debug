#pragma once

#include "Common.h"
#include <string>
#include <vector>
#include <cstdint>

namespace Aperture {

class MpvPlayer;
class Renderer;

namespace BuildInfo {

// --- Authoritative Typed Metadata Structures ---

struct VersionInfo {
    uint32_t major = 0, minor = 0, patch = 0, build = 0;
    std::string versionString, displayVersion, semVer, companyName;
    std::string legalCopyright, fileDescription, productName;
};

struct ToolchainInfo {
    std::string compilerName, compilerVersion, windowsSdkVersion, architecture;
    std::string cppStandard, buildConfiguration;
};

struct ProvenanceInfo { std::string commitSha, repository, channel; bool isCommitAvailable = false, isRepositoryAvailable = false; };

struct HardeningInfo {
    bool controlFlowGuard = true, shadowStackCet = true, dynamicBaseAslr = true, highEntropyVa = true;
    bool depNx = true, spectreMitigations = true, stackBufferSecurityCheck = true, sdlSecurityChecks = true;
    bool safeDllSearch = true, reproducibleBuild = true;
    std::vector<std::string> GetEnabledMitigationsList() const;
};

struct DependencyEntry { std::string id, name, category, version; std::string sha256, license, description; };

struct RuntimeCapabilities {
    bool portableRootWritable = true;
    std::wstring rootDirectory;
    bool isPrivateMode = false, updaterTrustConfigured = false;
    std::string updaterManifestUrl, updaterPinnedPublicKeyHex, updaterPinnedSignerThumbprint, libmpvVendoredVersion;
    std::string libmpvRuntimeVersion, libmpvClientApiVersion, libmpvAbiVersion;
    std::wstring rendererBackend, graphicsAdapterName, dwriteVersion, audioBackend;
};

struct BuildInfoSnapshot {
    VersionInfo version;
    ToolchainInfo toolchain;
    ProvenanceInfo provenance;
    HardeningInfo hardening;
    std::vector<DependencyEntry> dependencies;
    RuntimeCapabilities runtime;
};

// --- Snapshot & API Queries ---

BuildInfoSnapshot GetSnapshot( bool isPrivateMode = false, const MpvPlayer* player = nullptr, const Renderer* renderer = nullptr );

// --- Redaction & String Bounds ---

std::string RedactString(const std::string& input, bool redactPaths = true);
std::wstring RedactString(const std::wstring& input, bool redactPaths = true);

// --- Portable Root Writability Verification ---

bool CheckRootWritability(const std::wstring& root = GetExeDir());

// --- Bounded Serializers ---

std::string FormatJson(const BuildInfoSnapshot& snapshot, bool redactPaths = true);
std::wstring FormatHumanReadable(const BuildInfoSnapshot& snapshot, bool redactPaths = true);

// --- Diagnostic Support Actions ---

bool CopyDiagnosticsToClipboard( HWND hWnd, bool isPrivateMode, const MpvPlayer* player = nullptr, const Renderer* renderer = nullptr );

bool ExportDiagnosticsToFile( bool isPrivateMode, const MpvPlayer* player, const Renderer* renderer, std::wstring& outPath,
    std::string& outError );
} // namespace BuildInfo
} // namespace Aperture
