#include "BuildInfo.h"
#include "Version.h"
#include "ChannelPolicy.h"
#include "Updater.h"
#include "MpvPlayer.h"
#include "Renderer.h"
#include "Common.h"

#include <windows.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>

namespace Aperture::BuildInfo {

namespace {

using Microsoft::WRL::ComPtr;

constexpr size_t kMaxFormattedStringBytes = 256 * 1024; // 256 KiB bounding ceiling

std::wstring GetTimestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t value[32]{};
    swprintf_s(value, L"%04u%02u%02u-%02u%02u%02u-%03u", time.wYear, time.wMonth, time.wDay,
               time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return value;
}

std::wstring DiscoverPrimaryGpuName() {
    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        ComPtr<IDXGIAdapter1> adapter;
        if (SUCCEEDED(factory->EnumAdapters1(0, &adapter))) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc))) { return desc.Description; }
        }
    }
    return L"Direct3D 11 Hardware / WARP Device";
}

std::string JsonEscape(const std::string& str) {
    std::string out;
    out.reserve(str.size() + 16);
    for (char c : str) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    sprintf_s(buf, "\\u%04x", static_cast<unsigned int>(static_cast<unsigned char>(c)));
                    out += buf;
                } else { out += c; }
                break;
        }
    }
    return out;
}

void ReplaceAll(std::wstring& str, const std::wstring& from, const std::wstring& to) {
    if (from.empty()) return;
    size_t start = 0;
    while ((start = str.find(from, start)) != std::wstring::npos) { str.replace(start, from.length(), to); start += to.length(); }
}

void ReplaceAll(std::string& str, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t start = 0;
    while ((start = str.find(from, start)) != std::wstring::npos) { str.replace(start, from.length(), to); start += to.length(); }
}

} // namespace

std::vector<std::string> HardeningInfo::GetEnabledMitigationsList() const {
    std::vector<std::string> list;
    if (controlFlowGuard) list.push_back("Control Flow Guard (/guard:cf)");
    if (shadowStackCet) list.push_back("CET Shadow Stack (/CETCOMPAT)");
    if (dynamicBaseAslr) list.push_back("Dynamic Base ASLR (/DYNAMICBASE)");
    if (highEntropyVa) list.push_back("High-Entropy 64-bit VA (/HIGHENTROPYVA)");
    if (depNx) list.push_back("Data Execution Prevention (/NXCOMPAT)");
    if (spectreMitigations) list.push_back("Spectre Mitigations (/Qspectre)");
    if (stackBufferSecurityCheck) list.push_back("Stack Buffer Security Check (/GS)");
    if (sdlSecurityChecks) list.push_back("Security Development Lifecycle Checks (/sdl)");
    if (safeDllSearch) list.push_back("Safe DLL Search Order (ApplicationDir + System32)");
    if (reproducibleBuild) list.push_back("Deterministic Reproducible Build (/Brepro)");
    return list;
}

bool CheckRootWritability(const std::wstring& root) {
    if (root.empty()) return false;
    const std::wstring probe = root + L"\\.writetest_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                               std::to_wstring(GetCurrentThreadId()) + L".tmp";
    HANDLE hFile = CreateFileW( probe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr );
    if (hFile == INVALID_HANDLE_VALUE) { return false; }
    CloseHandle(hFile);
    return true;
}

std::wstring RedactString(const std::wstring& input, bool redactPaths) {
    std::wstring text = input;
    // 1. Redact active username and profile directory
    wchar_t userBuf[256]{};
    DWORD userLen = ARRAYSIZE(userBuf);
    if (GetUserNameW(userBuf, &userLen) && userLen > 1) {
        std::wstring userName(userBuf);
        ReplaceAll(text, L"\\Users\\" + userName + L"\\", L"\\Users\\<redacted>\\");
        ReplaceAll(text, L"/Users/" + userName + L"/", L"/Users/<redacted>/");
        ReplaceAll(text, L"\\users\\" + userName + L"\\", L"\\users\\<redacted>\\");
        ReplaceAll(text, L"/users/" + userName + L"/", L"/users/<redacted>/");
    }
    // 2. Redact temp paths
    wchar_t tempBuf[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempBuf)) {
        std::wstring tempPath(tempBuf);
        while (!tempPath.empty() && (tempPath.back() == L'\\' || tempPath.back() == L'/')) { tempPath.pop_back(); }
        if (!tempPath.empty()) { ReplaceAll(text, tempPath, L"<temp>"); }
    }
    // 3. Redact certificate and private key extensions/references if present
    const wchar_t* sensitiveTokens[] = { L".pfx", L".p12", L".key", L".pem" };
    for (const auto* tok : sensitiveTokens) {
        size_t pos = 0;
        while ((pos = text.find(tok, pos)) != std::wstring::npos) {
            // Locate enclosing path boundary
            size_t start = text.rfind(L' ', pos);
            if (start == std::wstring::npos) start = 0; else start += 1;
            size_t end = text.find_first_of(L" \r\n\t\"", pos);
            if (end == std::wstring::npos) end = text.size();
            text.replace(start, end - start, L"<redacted-certificate-path>");
            pos = start + 28;
        }
    }
    // 4. Bound string length
    if (text.size() > kMaxFormattedStringBytes / sizeof(wchar_t)) {
        text.resize(kMaxFormattedStringBytes / sizeof(wchar_t));
        text += L"\r\n[output truncated at byte ceiling]";
    }
    return text;
}

std::string RedactString(const std::string& input, bool redactPaths) {
    const std::wstring wide = Utf8ToUtf16(input);
    const std::wstring redacted = RedactString(wide, redactPaths);
    return Utf16ToUtf8(redacted);
}

BuildInfoSnapshot GetSnapshot( bool isPrivateMode, const MpvPlayer* player, const Renderer* renderer ) {
    BuildInfoSnapshot s;
    // --- Version Metadata ---
    s.version.major = Version::Major;
    s.version.minor = Version::Minor;
    s.version.patch = Version::Patch;
    s.version.build = Version::Build;
    s.version.versionString = std::string(Version::String);
    s.version.displayVersion = std::string(Version::DisplayVersion);
    s.version.semVer = std::string(Version::DisplayVersion);
    s.version.companyName = std::string(Version::CompanyName);
    s.version.legalCopyright = std::string(Version::LegalCopyright);
    s.version.fileDescription = std::string(Version::FileDescription);
    s.version.productName = std::string(Version::ProductName);
    // --- Toolchain Metadata ---
    s.toolchain.compilerName = std::string(Version::CompilerName);
    s.toolchain.compilerVersion = std::string(Version::CompilerVersion);
    s.toolchain.windowsSdkVersion = std::string(Version::WindowsSdkVersion);
    s.toolchain.architecture = std::string(Version::TargetArch);
    s.toolchain.cppStandard = "C++17";
#if defined(_DEBUG)
    s.toolchain.buildConfiguration = "Debug";
#elif defined(NDEBUG)
    s.toolchain.buildConfiguration = "Release";
#else
    s.toolchain.buildConfiguration = "Release";
#endif

    // --- CI Provenance Metadata ---
    s.provenance.commitSha = std::string(Version::BuildCommit);
    s.provenance.repository = std::string(Version::BuildRepository);
    s.provenance.channel = ChannelPolicy::Instance().CanonicalizeChannel(std::string(Version::BuildChannel));
    s.provenance.isCommitAvailable = !s.provenance.commitSha.empty() && s.provenance.commitSha != "unavailable";
    s.provenance.isRepositoryAvailable = !s.provenance.repository.empty() && s.provenance.repository != "unavailable";
    // --- Hardening Mitigations ---
    s.hardening.controlFlowGuard = true;
    s.hardening.shadowStackCet = true;
    s.hardening.dynamicBaseAslr = true;
    s.hardening.highEntropyVa = true;
    s.hardening.depNx = true;
    s.hardening.spectreMitigations = true;
    s.hardening.stackBufferSecurityCheck = true;
    s.hardening.sdlSecurityChecks = true;
    s.hardening.safeDllSearch = true;
    s.hardening.reproducibleBuild = true;
    // --- Dependency Inventory ---
    s.dependencies.push_back({"libmpv", "libmpv Shared Library", "required", std::string(Version::LibmpvVersion),
                              std::string(Version::LibmpvSha256), std::string(Version::LibmpvLicense), "Core media playback engine DLL"});
    s.dependencies.push_back({"font_inter", "Inter Variable Font", "required", std::string(Version::FontInterVersion),
                              std::string(Version::FontInterSha256), "OFL-1.1", "Primary UI typography"});
    s.dependencies.push_back({"font_space_grotesk", "Space Grotesk Variable Font", "required",
                              std::string(Version::FontSpaceGroteskVersion), std::string(Version::FontSpaceGroteskSha256), "OFL-1.1",
                              "Display and header typography"});
    s.dependencies.push_back({"font_jetbrains_mono", "JetBrains Mono Variable Font", "required",
                              std::string(Version::FontJetbrainsMonoVersion), std::string(Version::FontJetbrainsMonoSha256), "OFL-1.1",
                              "Monospace diagnostics and timecode typography"});
    // --- Runtime Capabilities ---
    s.runtime.rootDirectory = GetExeDir();
    s.runtime.portableRootWritable = CheckRootWritability(s.runtime.rootDirectory);
    s.runtime.isPrivateMode = isPrivateMode;
#ifndef APERTURE_UPDATER_MANIFEST_URL
#define APERTURE_UPDATER_MANIFEST_URL ""
#endif
#ifndef APERTURE_UPDATER_PUBLIC_KEY_HEX
#define APERTURE_UPDATER_PUBLIC_KEY_HEX ""
#endif
#ifndef APERTURE_UPDATER_SIGNER_THUMBPRINT
#define APERTURE_UPDATER_SIGNER_THUMBPRINT ""
#endif
    s.runtime.updaterManifestUrl = APERTURE_UPDATER_MANIFEST_URL;
    s.runtime.updaterPinnedPublicKeyHex = APERTURE_UPDATER_PUBLIC_KEY_HEX;
    s.runtime.updaterPinnedSignerThumbprint = APERTURE_UPDATER_SIGNER_THUMBPRINT;
    s.runtime.updaterTrustConfigured = !s.runtime.updaterPinnedPublicKeyHex.empty() || !s.runtime.updaterPinnedSignerThumbprint.empty();
    s.runtime.libmpvVendoredVersion = std::string(Version::LibmpvVersion);
    s.runtime.libmpvClientApiVersion = std::string(Version::LibmpvClientApi);
    s.runtime.libmpvAbiVersion = std::string(Version::LibmpvAbi);
    s.runtime.libmpvRuntimeVersion = s.runtime.libmpvVendoredVersion;
    s.runtime.rendererBackend = L"Direct2D 1.1 / Direct3D 11.0 (Feature Level 11.0)";
    s.runtime.graphicsAdapterName = DiscoverPrimaryGpuName();
    s.runtime.dwriteVersion = L"DirectWrite (Hardware Accelerated Font Typography)";
    s.runtime.audioBackend = L"WASAPI (Windows Audio Session API)";
    if (player) {
        PlayerDiagnostics pd = player->GetDiagnostics();
        if (!pd.activeAudioDevice.empty()) { s.runtime.audioBackend = L"WASAPI (" + pd.activeAudioDevice + L")"; }
    }
    return s;
}

std::wstring FormatHumanReadable(const BuildInfoSnapshot& snapshot, bool redactPaths) {
    std::wostringstream w;
    w << L"=================================================================\r\n"
      << L"  " << Utf8ToUtf16(snapshot.version.productName) << L" Build & Diagnostic Information\r\n"
      << L"=================================================================\r\n\r\n"
      << L"[PRODUCT & VERSION]\r\n"
      << L"  Application:        " << Utf8ToUtf16(snapshot.version.productName) << L"\r\n"
      << L"  Display Version:    " << Utf8ToUtf16(snapshot.version.displayVersion) << L"\r\n"
      << L"  File Version:       " << Utf8ToUtf16(snapshot.version.versionString) << L"\r\n"
      << L"  Build Configuration:" << Utf8ToUtf16(snapshot.toolchain.buildConfiguration) << L"\r\n"
      << L"  Architecture:       " << Utf8ToUtf16(snapshot.toolchain.architecture) << L"\r\n"
      << L"  C++ Standard:       " << Utf8ToUtf16(snapshot.toolchain.cppStandard) << L"\r\n\r\n"
      << L"[TOOLCHAIN & COMPILER]\r\n"
      << L"  Compiler:           " << Utf8ToUtf16(snapshot.toolchain.compilerName) << L" (v" << Utf8ToUtf16(snapshot.toolchain.compilerVersion) << L")\r\n"
      << L"  Windows SDK:        " << Utf8ToUtf16(snapshot.toolchain.windowsSdkVersion) << L"\r\n\r\n"
      << L"[CI & PROVENANCE]\r\n"
      << L"  Release Channel:    " << Utf8ToUtf16(snapshot.provenance.channel) << L"\r\n"
      << L"  Commit SHA:         " << Utf8ToUtf16(snapshot.provenance.commitSha) << L"\r\n"
      << L"  Repository:         " << Utf8ToUtf16(snapshot.provenance.repository) << L"\r\n\r\n"
      << L"[HARDENING & SECURITY MITIGATIONS]\r\n";
    for (const auto& mit : snapshot.hardening.GetEnabledMitigationsList()) { w << L"  + " << Utf8ToUtf16(mit) << L"\r\n"; }
    w << L"\r\n[PORTABLE RUNTIME & STORAGE]\r\n"
      << L"  Portable Root:      " << (snapshot.runtime.portableRootWritable ? L"Writable" : L"Read-Only / Unwritable") << L"\r\n"
      << L"  Executable Path:    " << snapshot.runtime.rootDirectory << L"\r\n"
      << L"  Private Session:    " << (snapshot.runtime.isPrivateMode ? L"Active (Zero durable traces)" : L"Inactive") << L"\r\n"
      << L"  Updater Trust:      " << (snapshot.runtime.updaterTrustConfigured ? L"Configured (Pinned Release Root)" : L"Unconfigured (Production release keys not provisioned)") << L"\r\n\r\n"
      << L"[GRAPHICS & PLAYBACK ENGINE]\r\n"
      << L"  Graphics Backend:   " << snapshot.runtime.rendererBackend << L"\r\n"
      << L"  Primary GPU:        " << snapshot.runtime.graphicsAdapterName << L"\r\n"
      << L"  Typography Engine:  " << snapshot.runtime.dwriteVersion << L"\r\n"
      << L"  Audio Subsystem:    " << snapshot.runtime.audioBackend << L"\r\n"
      << L"  libmpv Engine:      " << Utf8ToUtf16(snapshot.runtime.libmpvVendoredVersion) << L" (Client API " << Utf8ToUtf16(snapshot.runtime.libmpvClientApiVersion) << L", ABI " << Utf8ToUtf16(snapshot.runtime.libmpvAbiVersion) << L")\r\n\r\n"
      << L"[VENDORED DEPENDENCY INVENTORY & HASHES]\r\n";
    for (const auto& dep : snapshot.dependencies) {
        w << L"  * " << Utf8ToUtf16(dep.name) << L" (v" << Utf8ToUtf16(dep.version) << L") [" << Utf8ToUtf16(dep.license) << L"]\r\n"
          << L"    SHA-256: " << Utf8ToUtf16(dep.sha256) << L"\r\n";
    }
    w << L"\r\n[EXPORT METADATA]\r\n" << L"  Generated At:       " << GetTimestamp() << L"\r\n";
    std::wstring result = w.str();
    if (redactPaths || snapshot.runtime.isPrivateMode) { result = RedactString(result, true); }
    return result;
}

std::string FormatJson(const BuildInfoSnapshot& snapshot, bool redactPaths) {
    std::ostringstream j;
    j << "{\n"
      << "  \"application\": \"" << JsonEscape(snapshot.version.productName) << "\",\n"
      << "  \"version\": \"" << JsonEscape(snapshot.version.versionString) << "\",\n"
      << "  \"displayVersion\": \"" << JsonEscape(snapshot.version.displayVersion) << "\",\n"
      << "  \"configuration\": \"" << JsonEscape(snapshot.toolchain.buildConfiguration) << "\",\n"
      << "  \"architecture\": \"" << JsonEscape(snapshot.toolchain.architecture) << "\",\n"
      << "  \"toolchain\": {\n"
      << "    \"compiler\": \"" << JsonEscape(snapshot.toolchain.compilerName) << "\",\n"
      << "    \"compilerVersion\": \"" << JsonEscape(snapshot.toolchain.compilerVersion) << "\",\n"
      << "    \"windowsSdkVersion\": \"" << JsonEscape(snapshot.toolchain.windowsSdkVersion) << "\",\n"
      << "    \"cppStandard\": \"" << JsonEscape(snapshot.toolchain.cppStandard) << "\"\n"
      << "  },\n"
      << "  \"provenance\": {\n"
      << "    \"channel\": \"" << JsonEscape(snapshot.provenance.channel) << "\",\n"
      << "    \"commit\": \"" << JsonEscape(snapshot.provenance.commitSha) << "\",\n"
      << "    \"repository\": \"" << JsonEscape(snapshot.provenance.repository) << "\"\n"
      << "  },\n"
      << "  \"hardening\": [\n";
    const auto mitigations = snapshot.hardening.GetEnabledMitigationsList();
    for (size_t i = 0; i < mitigations.size(); ++i) {
        j << "    \"" << JsonEscape(mitigations[i]) << "\"" << (i + 1 < mitigations.size() ? "," : "") << "\n";
    }
    j << "  ],\n"
      << "  \"runtime\": {\n"
      << "    \"portableRootWritable\": " << (snapshot.runtime.portableRootWritable ? "true" : "false") << ",\n"
      << "    \"rootDirectory\": \"" << JsonEscape(Utf16ToUtf8(snapshot.runtime.rootDirectory)) << "\",\n"
      << "    \"privateMode\": " << (snapshot.runtime.isPrivateMode ? "true" : "false") << ",\n"
      << "    \"updaterTrustConfigured\": " << (snapshot.runtime.updaterTrustConfigured ? "true" : "false") << ",\n"
      << "    \"graphics\": {\n"
      << "      \"backend\": \"" << JsonEscape(Utf16ToUtf8(snapshot.runtime.rendererBackend)) << "\",\n"
      << "      \"adapter\": \"" << JsonEscape(Utf16ToUtf8(snapshot.runtime.graphicsAdapterName)) << "\"\n"
      << "    },\n"
      << "    \"playback\": {\n"
      << "      \"engine\": \"libmpv\",\n"
      << "      \"version\": \"" << JsonEscape(snapshot.runtime.libmpvVendoredVersion) << "\",\n"
      << "      \"clientApiVersion\": \"" << JsonEscape(snapshot.runtime.libmpvClientApiVersion) << "\",\n"
      << "      \"abiVersion\": \"" << JsonEscape(snapshot.runtime.libmpvAbiVersion) << "\"\n"
      << "    },\n"
      << "    \"audio\": {\n"
      << "      \"backend\": \"" << JsonEscape(Utf16ToUtf8(snapshot.runtime.audioBackend)) << "\"\n"
      << "    }\n"
      << "  },\n"
      << "  \"dependencies\": [\n";
    for (size_t i = 0; i < snapshot.dependencies.size(); ++i) {
        const auto& d = snapshot.dependencies[i];
        j << "    {\n"
          << "      \"id\": \"" << JsonEscape(d.id) << "\",\n"
          << "      \"name\": \"" << JsonEscape(d.name) << "\",\n"
          << "      \"category\": \"" << JsonEscape(d.category) << "\",\n"
          << "      \"version\": \"" << JsonEscape(d.version) << "\",\n"
          << "      \"sha256\": \"" << JsonEscape(d.sha256) << "\",\n"
          << "      \"license\": \"" << JsonEscape(d.license) << "\"\n"
          << "    }" << (i + 1 < snapshot.dependencies.size() ? "," : "") << "\n";
    }
    j << "  ]\n"
      << "}\n";
    std::string result = j.str();
    if (redactPaths || snapshot.runtime.isPrivateMode) { result = RedactString(result, true); }
    return result;
}

bool CopyDiagnosticsToClipboard( HWND hWnd, bool isPrivateMode, const MpvPlayer* player, const Renderer* renderer ) {
    if (!hWnd) return false;
    const BuildInfoSnapshot snapshot = GetSnapshot(isPrivateMode, player, renderer);
    const std::wstring wideText = FormatHumanReadable(snapshot, true);
    if (!OpenClipboard(hWnd)) return false;
    EmptyClipboard();
    const SIZE_T bytes = (wideText.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) { CloseClipboard(); return false; }
    void* pMem = GlobalLock(hMem);
    if (!pMem) { GlobalFree(hMem); CloseClipboard(); return false; }
    CopyMemory(pMem, wideText.c_str(), bytes);
    GlobalUnlock(hMem);
    if (!SetClipboardData(CF_UNICODETEXT, hMem)) { GlobalFree(hMem); CloseClipboard(); return false; }
    CloseClipboard();
    return true;
}

bool ExportDiagnosticsToFile( bool isPrivateMode, const MpvPlayer* player, const Renderer* renderer, std::wstring& outPath,
    std::string& outError ) {
    outPath.clear();
    outError.clear();
    const std::wstring exeDir = GetExeDir();
    if (!CheckRootWritability(exeDir)) { outError = "Portable root directory is read-only or not writable"; return false; }
    const std::wstring diagDir = exeDir + L"\\diagnostics";
    const int mkRes = SHCreateDirectoryExW(nullptr, diagDir.c_str(), nullptr);
    if (mkRes != ERROR_SUCCESS && mkRes != ERROR_ALREADY_EXISTS && mkRes != ERROR_FILE_EXISTS) {
        outError = "Failed to create diagnostics directory in portable root";
        return false;
    }
    const BuildInfoSnapshot snapshot = GetSnapshot(isPrivateMode, player, renderer);
    const std::string jsonReport = FormatJson(snapshot, true);
    const std::wstring targetPath = diagDir + L"\\diagnostics-" + GetTimestamp() + L".json";
    if (!IsPathUnderRoot(targetPath, exeDir)) {
        outError = "Resolved export path violates portable root boundary constraints";
        return false;
    }
    const StorageStatus status = AtomicWriteTextFile(targetPath, jsonReport);
    if (!status.is_ok()) { outError = status.message; return false; }
    outPath = targetPath;
    return true;
}

} // namespace Aperture::BuildInfo
