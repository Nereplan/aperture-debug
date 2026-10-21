#include "Updater.h"
#include "Version.h"
#include "Json.h"
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <ctime>
#include <cctype>
#include <random>
#include <filesystem>
#include <limits>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "winhttp.lib")

namespace Aperture {

namespace fs = std::filesystem;

// --- Build-Time Compile Definitions ---
#ifndef APERTURE_UPDATER_MANIFEST_URL
#define APERTURE_UPDATER_MANIFEST_URL ""
#endif

#ifndef APERTURE_UPDATER_STABLE_MANIFEST_URL
#define APERTURE_UPDATER_STABLE_MANIFEST_URL APERTURE_UPDATER_MANIFEST_URL
#endif

#ifndef APERTURE_UPDATER_BETA_MANIFEST_URL
#define APERTURE_UPDATER_BETA_MANIFEST_URL ""
#endif

#ifndef APERTURE_UPDATER_NIGHTLY_MANIFEST_URL
#define APERTURE_UPDATER_NIGHTLY_MANIFEST_URL ""
#endif

#ifndef APERTURE_UPDATER_ALLOWED_HOSTS
#define APERTURE_UPDATER_ALLOWED_HOSTS ""
#endif

#ifndef APERTURE_UPDATER_PUBLIC_KEY_HEX
#define APERTURE_UPDATER_PUBLIC_KEY_HEX ""
#endif

#ifndef APERTURE_UPDATER_STABLE_PUBLIC_KEY_HEX
#define APERTURE_UPDATER_STABLE_PUBLIC_KEY_HEX APERTURE_UPDATER_PUBLIC_KEY_HEX
#endif

#ifndef APERTURE_UPDATER_BETA_PUBLIC_KEY_HEX
#define APERTURE_UPDATER_BETA_PUBLIC_KEY_HEX ""
#endif

#ifndef APERTURE_UPDATER_NIGHTLY_PUBLIC_KEY_HEX
#define APERTURE_UPDATER_NIGHTLY_PUBLIC_KEY_HEX ""
#endif

#ifndef APERTURE_UPDATER_SIGNER_THUMBPRINT
#define APERTURE_UPDATER_SIGNER_THUMBPRINT ""
#endif

#ifndef APERTURE_UPDATER_STABLE_SIGNER_THUMBPRINT
#define APERTURE_UPDATER_STABLE_SIGNER_THUMBPRINT APERTURE_UPDATER_SIGNER_THUMBPRINT
#endif

#ifndef APERTURE_UPDATER_BETA_SIGNER_THUMBPRINT
#define APERTURE_UPDATER_BETA_SIGNER_THUMBPRINT ""
#endif

#ifndef APERTURE_UPDATER_NIGHTLY_SIGNER_THUMBPRINT
#define APERTURE_UPDATER_NIGHTLY_SIGNER_THUMBPRINT ""
#endif

// --- String & Hex Helpers ---

static std::string ToHex(const uint8_t* data, size_t len) {
    static const char hexChars[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) { hex.push_back(hexChars[(data[i] >> 4) & 0x0F]); hex.push_back(hexChars[data[i] & 0x0F]); }
    return hex;
}

static bool FromHex(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.length() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.length() / 2);
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.length(); i += 2) {
        int hi = hexVal(hex[i]);
        int lo = hexVal(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

static std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

static bool CaseInsensitiveEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) { return false; }
    }
    return true;
}

// --- BuildTimeTrustConfig Implementation ---

std::string BuildTimeTrustConfig::GetManifestUrl(const std::string& channel) {
    std::string ch = channel.empty() ? ChannelPolicy::Instance().GetDefaultChannel() : ChannelPolicy::Instance().CanonicalizeChannel(channel);
    if (ch == "beta") { std::string url = APERTURE_UPDATER_BETA_MANIFEST_URL; if (!url.empty()) return url; } else if (ch == "nightly") {
        std::string url = APERTURE_UPDATER_NIGHTLY_MANIFEST_URL;
        if (!url.empty()) return url;
    } else if (ch == "stable") { std::string url = APERTURE_UPDATER_STABLE_MANIFEST_URL; if (!url.empty()) return url; }
    return APERTURE_UPDATER_MANIFEST_URL;
}

std::vector<std::string> BuildTimeTrustConfig::GetAllowedHosts(const std::string& channel) {
    std::string hostsStr = APERTURE_UPDATER_ALLOWED_HOSTS;
    std::vector<std::string> list;
    std::istringstream ss(hostsStr);
    std::string item;
    while (std::getline(ss, item, ';')) { item = Trim(item); if (!item.empty()) list.push_back(item); }
    return list;
}

std::string BuildTimeTrustConfig::GetPinnedPublicKeyHex(const std::string& channel) {
    std::string ch = channel.empty() ? ChannelPolicy::Instance().GetDefaultChannel() : ChannelPolicy::Instance().CanonicalizeChannel(channel);
    if (ch == "beta") { std::string key = APERTURE_UPDATER_BETA_PUBLIC_KEY_HEX; if (!key.empty()) return key; } else if (ch == "nightly") {
        std::string key = APERTURE_UPDATER_NIGHTLY_PUBLIC_KEY_HEX;
        if (!key.empty()) return key;
    } else if (ch == "stable") { std::string key = APERTURE_UPDATER_STABLE_PUBLIC_KEY_HEX; if (!key.empty()) return key; }
    return APERTURE_UPDATER_PUBLIC_KEY_HEX;
}

std::string BuildTimeTrustConfig::GetPinnedSignerThumbprint(const std::string& channel) {
    std::string ch = channel.empty() ? ChannelPolicy::Instance().GetDefaultChannel() : ChannelPolicy::Instance().CanonicalizeChannel(channel);
    if (ch == "beta") {
        std::string thumb = APERTURE_UPDATER_BETA_SIGNER_THUMBPRINT;
        if (!thumb.empty()) return thumb;
    } else if (ch == "nightly") {
        std::string thumb = APERTURE_UPDATER_NIGHTLY_SIGNER_THUMBPRINT;
        if (!thumb.empty()) return thumb;
    } else if (ch == "stable") { std::string thumb = APERTURE_UPDATER_STABLE_SIGNER_THUMBPRINT; if (!thumb.empty()) return thumb; }
    return APERTURE_UPDATER_SIGNER_THUMBPRINT;
}

// --- UpdaterConfig Implementation ---

UpdaterConfig::UpdaterConfig(const std::string& channelName) {
    channel = ChannelPolicy::Instance().CanonicalizeChannel(channelName);
    manifestUrl = BuildTimeTrustConfig::GetManifestUrl(channel);
    allowedHosts = BuildTimeTrustConfig::GetAllowedHosts(channel);
}

bool UpdaterConfig::IsTrustConfigured() const {
    if (allowUnsignedForTesting) return !GetEffectiveManifestUrl().empty();
    std::string url = GetEffectiveManifestUrl();
    std::string pubKey = GetEffectivePublicKeyHex();
    std::string thumb = GetEffectiveSignerThumbprint();
    return !url.empty() && url.rfind("https://", 0) == 0 && !pubKey.empty() && !thumb.empty();
}

std::string UpdaterConfig::GetEffectivePublicKeyHex() const {
    if (!overridePinnedPublicKeyHex.empty()) return overridePinnedPublicKeyHex;
    return BuildTimeTrustConfig::GetPinnedPublicKeyHex(channel);
}

std::string UpdaterConfig::GetEffectiveSignerThumbprint() const {
    if (!overridePinnedSignerThumbprint.empty()) return overridePinnedSignerThumbprint;
    return BuildTimeTrustConfig::GetPinnedSignerThumbprint(channel);
}

std::string UpdaterConfig::GetEffectiveManifestUrl() const {
    if (!manifestUrl.empty()) return manifestUrl;
    return BuildTimeTrustConfig::GetManifestUrl(channel);
}

// --- ISO 8601 UTC Timestamp Parsing & Formatting ---

bool ParseIso8601Utc(const std::string& str, std::chrono::system_clock::time_point& outTime) {
    std::string s = Trim(str);
    if (s.empty()) return false;
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    char sep1 = 0, sep2 = 0, sepT = 0, sep3 = 0, sep4 = 0;
    std::istringstream ss(s);
    ss >> year >> sep1 >> month >> sep2 >> day >> sepT >> hour >> sep3 >> min >> sep4 >> sec;
    if (ss.fail() || sep1 != '-' || sep2 != '-' || (sepT != 'T' && sepT != 't') || sep3 != ':' || sep4 != ':') { return false; }
    if (year < 1970 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60) {
        return false;
    }
    std::tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    tm.tm_isdst = 0;
    time_t t = _mkgmtime(&tm);
    if (t == (time_t)-1) return false;
    outTime = std::chrono::system_clock::from_time_t(t);
    return true;
}

std::string FormatIso8601Utc(std::chrono::system_clock::time_point tp) {
    time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm = {};
    gmtime_s(&tm, &t);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

// --- Semantic Version Implementation ---

std::optional<SemanticVersion> SemanticVersion::Parse(const std::string& str) {
    std::string s = Trim(str);
    if (s.empty()) return std::nullopt;
    if (s.front() == 'v' || s.front() == 'V') { s = s.substr(1); }
    std::string verPart = s;
    std::string prePart, buildPart;
    // Split build metadata after '+'
    size_t plus = verPart.find('+');
    if (plus != std::string::npos) { buildPart = verPart.substr(plus + 1); verPart = verPart.substr(0, plus); }
    // Split prerelease after '-'
    size_t dash = verPart.find('-');
    if (dash != std::string::npos) { prePart = verPart.substr(dash + 1); verPart = verPart.substr(0, dash); }
    std::vector<int> nums;
    std::istringstream ss(verPart);
    std::string token;
    while (std::getline(ss, token, '.')) {
        if (token.empty()) return std::nullopt;
        for (char c : token) { if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt; }
        try { nums.push_back(std::stoi(token)); } catch (...) { return std::nullopt; }
    }
    if (nums.empty() || nums.size() > 4) return std::nullopt;
    SemanticVersion v;
    v.major = nums.size() > 0 ? nums[0] : 0;
    v.minor = nums.size() > 1 ? nums[1] : 0;
    v.patch = nums.size() > 2 ? nums[2] : 0;
    v.build = nums.size() > 3 ? nums[3] : 0;
    v.preRelease = prePart;
    v.buildMetadata = buildPart;
    return v;
}

std::string SemanticVersion::ToString() const {
    std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    if (build > 0) { s += "." + std::to_string(build); }
    if (!preRelease.empty()) { s += "-" + preRelease; }
    if (!buildMetadata.empty()) { s += "+" + buildMetadata; }
    return s;
}

bool SemanticVersion::operator<(const SemanticVersion& other) const {
    if (major != other.major) return major < other.major;
    if (minor != other.minor) return minor < other.minor;
    if (patch != other.patch) return patch < other.patch;
    if (build != other.build) return build < other.build;
    return SemVerUtils::ComparePrerelease(preRelease, other.preRelease) < 0;
}

bool SemanticVersion::operator<=(const SemanticVersion& other) const { return (*this < other) || (*this == other); }

bool SemanticVersion::operator==(const SemanticVersion& other) const {
    return major == other.major && minor == other.minor && patch == other.patch && build == other.build &&
           SemVerUtils::ComparePrerelease(preRelease, other.preRelease) == 0;
}

bool SemanticVersion::operator!=(const SemanticVersion& other) const { return !(*this == other); }

bool SemanticVersion::operator>=(const SemanticVersion& other) const { return !(*this < other); }

bool SemanticVersion::operator>(const SemanticVersion& other) const { return !(*this <= other); }

SemanticVersion SemanticVersion::Current() {
    return {
        static_cast<int>(Version::Major), static_cast<int>(Version::Minor), static_cast<int>(Version::Patch),
        static_cast<int>(Version::Build), "", ""
    };
}

// --- Canonical Release Manifest Implementation ---

std::string ReleaseManifest::ComputeCanonicalString() const {
    std::ostringstream oss;
    oss << "Aperture-Release-Manifest-V1\n";
    oss << "application:" << application << "\n";
    oss << "version:" << version << "\n";
    oss << "channel:" << channel << "\n";
    if (!minimumUpdaterVersion.empty()) { oss << "minimumUpdaterVersion:" << minimumUpdaterVersion << "\n"; }
    oss << "publishedUtc:" << publishedUtc << "\n";
    oss << "expiryUtc:" << expiryUtc << "\n";
    oss << "downloadUrl:" << downloadUrl << "\n";
    oss << "fileSizeBytes:" << fileSizeBytes << "\n";
    oss << "sha256:" << sha256 << "\n";
    if (!signerThumbprint.empty()) { oss << "signerThumbprint:" << signerThumbprint << "\n"; }
    if (!files.empty()) {
        auto sortedFiles = files;
        std::sort(sortedFiles.begin(), sortedFiles.end(),
                  [](const ManifestFileEntry &a, const ManifestFileEntry &b) { return a.path < b.path; });
        for (const auto& f : sortedFiles) { oss << "file:" << f.path << ":" << f.size << ":" << f.sha256 << "\n"; }
    }
    return oss.str();
}

bool ReleaseManifest::Parse(const std::string& jsonText, ReleaseManifest& outManifest, std::string& outError) {
    outError.clear();
    JsonValue root;
    if (!JsonValue::parse(jsonText, root) || !root.is_object()) { outError = "Malformed JSON syntax"; return false; }
    outManifest = ReleaseManifest();
    outManifest.rawJson = jsonText;
    outManifest.extraFields = root;
    outManifest.schemaVersion = root.get("schemaVersion").as_int(1);
    outManifest.application = root.get("application").as_string("Aperture");
    outManifest.version = root.get("version").as_string("");
    outManifest.channel = root.get("channel").as_string("stable");
    outManifest.minimumUpdaterVersion = root.get("minimumUpdaterVersion").as_string("");
    outManifest.publishedUtc = root.get("publishedUtc").as_string("");
    outManifest.expiryUtc = root.get("expiryUtc").as_string("");
    outManifest.downloadUrl = root.get("downloadUrl").as_string("");
    double sz = root.get("fileSizeBytes").as_number(0.0);
    outManifest.fileSizeBytes = static_cast<uint64_t>(sz >= 0 ? sz : 0);
    outManifest.sha256 = root.get("sha256").as_string("");
    outManifest.signerThumbprint = root.get("signerThumbprint").as_string("");
    outManifest.signature = root.get("signature").as_string("");
    outManifest.signatureAlgorithm = root.get("signatureAlgorithm").as_string("ECDSA_P256_SHA256");
    outManifest.releaseNotes = root.get("releaseNotes").as_string("");
    if (root.has_key("files") && root.get("files").is_array()) {
        const auto& arr = root.get("files").as_array();
        for (const auto& item : arr) {
            if (item.is_object()) {
                ManifestFileEntry entry;
                entry.path = item.get("path").as_string("");
                double fsz = item.get("size").as_number(0.0);
                entry.size = static_cast<uint64_t>(fsz >= 0 ? fsz : 0);
                entry.sha256 = item.get("sha256").as_string("");
                if (!entry.path.empty()) { outManifest.files.push_back(entry); }
            }
        }
    }
    if (outManifest.version.empty()) { outError = "Missing required field 'version'"; return false; }
    if (outManifest.downloadUrl.empty()) { outError = "Missing required field 'downloadUrl'"; return false; }
    if (outManifest.sha256.empty()) { outError = "Missing required field 'sha256'"; return false; }
    return true;
}

std::string ReleaseManifest::SerializeJson() const {
    JsonValue root = extraFields.is_object() ? extraFields : JsonValue::Object();
    root["schemaVersion"] = schemaVersion;
    root["application"] = application;
    root["version"] = version;
    root["channel"] = channel;
    if (!minimumUpdaterVersion.empty()) { root["minimumUpdaterVersion"] = minimumUpdaterVersion; }
    root["publishedUtc"] = publishedUtc;
    root["expiryUtc"] = expiryUtc;
    root["downloadUrl"] = downloadUrl;
    root["fileSizeBytes"] = static_cast<double>(fileSizeBytes);
    root["sha256"] = sha256;
    if (!signerThumbprint.empty()) { root["signerThumbprint"] = signerThumbprint; }
    if (!signature.empty()) { root["signature"] = signature; root["signatureAlgorithm"] = signatureAlgorithm; }
    if (!releaseNotes.empty()) { root["releaseNotes"] = releaseNotes; }
    if (!files.empty()) {
        JsonValue filesArr = JsonValue::Array();
        for (const auto& f : files) {
            JsonValue obj = JsonValue::Object();
            obj["path"] = f.path;
            obj["size"] = static_cast<double>(f.size);
            obj["sha256"] = f.sha256;
            filesArr.push_back(obj);
        }
        root["files"] = filesArr;
    }
    return root.stringify(true);
}

// --- Cryptographic & Hashing Implementations ---

std::string ComputeSha256Hex(const void* data, size_t size) {
    if ((!data && size > 0) || size > std::numeric_limits<ULONG>::max()) return "";
    // bcrypt is delay-loaded: system-only presence check first.
    if (!SystemDllPresent(L"bcrypt.dll")) return "";
    BCRYPT_ALG_HANDLE hAlg = NULL;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (!BCRYPT_SUCCESS(status)) return "";
    DWORD hashObjectSize = 0, dataSize = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&hashObjectSize, sizeof(DWORD), &dataSize, 0))) {
        BCryptCloseAlgorithmProvider(hAlg, 0); return "";
    }
    std::vector<uint8_t> hashObject(hashObjectSize);
    BCRYPT_HASH_HANDLE hHash = NULL;
    status = BCryptCreateHash(hAlg, &hHash, hashObject.data(), hashObjectSize, NULL, 0, 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); return ""; }
    uint8_t hash[32] = { 0 };
    const bool hashOk = (size == 0 || BCRYPT_SUCCESS(BCryptHashData(hHash, (PUCHAR)data, static_cast<ULONG>(size), 0))) &&
        BCRYPT_SUCCESS(BCryptFinishHash(hHash, hash, sizeof(hash), 0));
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return hashOk ? ToHex(hash, sizeof(hash)) : "";
}

std::string ComputeFileSha256Hex(const std::wstring& path) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return "";
    if (!SystemDllPresent(L"bcrypt.dll")) { CloseHandle(hFile); return ""; }
    BCRYPT_ALG_HANDLE hAlg = NULL;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (!BCRYPT_SUCCESS(status)) { CloseHandle(hFile); return ""; }
    DWORD hashObjectSize = 0, dataSize = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&hashObjectSize, sizeof(DWORD), &dataSize, 0))) {
        BCryptCloseAlgorithmProvider(hAlg, 0); CloseHandle(hFile); return "";
    }
    std::vector<uint8_t> hashObject(hashObjectSize);
    BCRYPT_HASH_HANDLE hHash = NULL;
    status = BCryptCreateHash(hAlg, &hHash, hashObject.data(), hashObjectSize, NULL, 0, 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); CloseHandle(hFile); return ""; }
    std::vector<uint8_t> buffer(64 * 1024);
    DWORD bytesRead = 0;
    bool readOk = true;
    for (;;) {
        if (!ReadFile(hFile, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, NULL)) { readOk = false; break; }
        if (!bytesRead) break;
        if (!BCRYPT_SUCCESS(BCryptHashData(hHash, buffer.data(), bytesRead, 0))) { readOk = false; break; }
    }
    uint8_t hash[32] = { 0 };
    const bool hashOk = readOk && BCRYPT_SUCCESS(BCryptFinishHash(hHash, hash, sizeof(hash), 0));
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    CloseHandle(hFile);
    return hashOk ? ToHex(hash, sizeof(hash)) : "";
}

bool VerifyEcdsaP256Signature(const std::string& canonicalData, const std::string& signatureHexOrBase64, const std::vector<uint8_t>& publicKeyBlob) {
    if (publicKeyBlob.empty() || signatureHexOrBase64.empty()) return false;
    if (!SystemDllPresent(L"bcrypt.dll")) return false;
    std::vector<uint8_t> sigBytes;
    if (!FromHex(signatureHexOrBase64, sigBytes)) { return false; }
    if (sigBytes.size() != 64) { return false; }
    BCRYPT_ALG_HANDLE hAlg = NULL;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0);
    if (!BCRYPT_SUCCESS(status)) return false;
    BCRYPT_KEY_HANDLE hKey = NULL;
    status = BCryptImportKeyPair( hAlg, NULL, BCRYPT_ECCPUBLIC_BLOB, &hKey,
        const_cast<PUCHAR>(publicKeyBlob.data()), static_cast<ULONG>(publicKeyBlob.size()), 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); return false; }
    std::string hashHex = ComputeSha256Hex(canonicalData.data(), canonicalData.size());
    std::vector<uint8_t> hashBytes;
    FromHex(hashHex, hashBytes);
    status = BCryptVerifySignature( hKey, NULL, hashBytes.data(), static_cast<ULONG>(hashBytes.size()),
        sigBytes.data(), static_cast<ULONG>(sigBytes.size()), 0);
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return BCRYPT_SUCCESS(status);
}

// --- Authenticode & PE Architecture Inspection ---

bool VerifyAuthenticodeAndPin( const std::wstring& filePath, const std::string& expectedThumbprintHex, std::string& outActualThumbprint,
    std::string &outError) {
    outActualThumbprint.clear();
    outError.clear();
    if (!FileExists(filePath)) { outError = "Binary file not found"; return false; }
    // wintrust/crypt32 are delay-loaded: system-only presence first.
    if (!SystemDllPresent(L"wintrust.dll") || !SystemDllPresent(L"crypt32.dll")) {
        outError = "Authenticode verification is unavailable on this system";
        return false;
    }
    WINTRUST_FILE_INFO fileInfo = {};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = filePath.c_str();
    WINTRUST_DATA trustData = {};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    trustData.dwProvFlags = WTD_SAFER_FLAG;
    GUID actionGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status = WinVerifyTrust(NULL, &actionGuid, &trustData);
    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &actionGuid, &trustData);
    if (status != ERROR_SUCCESS) {
        if (status == TRUST_E_NOSIGNATURE) {
            outError = "File is not digitally signed (TRUST_E_NOSIGNATURE)";
        } else { outError = "Authenticode validation failed with status 0x" + ToHex((const uint8_t*)&status, sizeof(status)); }
    }
    HCERTSTORE hStore = NULL;
    HCRYPTMSG hMsg = NULL;
    BOOL queryOk = CryptQueryObject( CERT_QUERY_OBJECT_FILE, filePath.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
        CERT_QUERY_FORMAT_FLAG_BINARY, 0, NULL, NULL, NULL, &hStore, &hMsg, NULL);
    if (!queryOk || !hMsg || !hStore) {
        if (hStore) CertCloseStore(hStore, 0);
        if (hMsg) CryptMsgClose(hMsg);
        if (outError.empty()) outError = "Could not extract PKCS#7 signature from file";
        return false;
    }
    DWORD signerInfoLen = 0;
    if (!CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &signerInfoLen)) {
        CertCloseStore(hStore, 0);
        CryptMsgClose(hMsg);
        outError = "Could not read signer info from signature";
        return false;
    }
    std::vector<uint8_t> signerInfoBuf(signerInfoLen);
    if (!CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, signerInfoBuf.data(), &signerInfoLen)) {
        CertCloseStore(hStore, 0);
        CryptMsgClose(hMsg);
        outError = "Could not retrieve signer info structure";
        return false;
    }
    CMSG_SIGNER_INFO* pSignerInfo = reinterpret_cast<CMSG_SIGNER_INFO*>(signerInfoBuf.data());
    CERT_INFO certInfo = {};
    certInfo.Issuer = pSignerInfo->Issuer;
    certInfo.SerialNumber = pSignerInfo->SerialNumber;
    PCCERT_CONTEXT pCertContext = CertFindCertificateInStore( hStore, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT,
        &certInfo, NULL);
    if (!pCertContext) {
        CertCloseStore(hStore, 0);
        CryptMsgClose(hMsg);
        outError = "Signer certificate not found in signature store";
        return false;
    }
    outActualThumbprint = ComputeSha256Hex(pCertContext->pbCertEncoded, pCertContext->cbCertEncoded);
    CertFreeCertificateContext(pCertContext);
    CertCloseStore(hStore, 0);
    CryptMsgClose(hMsg);
    if (!expectedThumbprintHex.empty()) {
        if (!CaseInsensitiveEqual(outActualThumbprint, expectedThumbprintHex)) {
            outError = "Authenticode certificate thumbprint mismatch: expected " + expectedThumbprintHex + ", found " + outActualThumbprint;
            return false;
        }
    }
    return true;
}

bool VerifyPeArchitectureX64(const std::wstring& filePath, std::string& outError) {
    outError.clear();
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) { outError = "Failed to open PE file for header inspection"; return false; }
    IMAGE_DOS_HEADER dosHdr = {};
    DWORD read = 0;
    if (!ReadFile(hFile, &dosHdr, sizeof(dosHdr), &read, NULL) || read != sizeof(dosHdr) || dosHdr.e_magic != IMAGE_DOS_SIGNATURE) {
        CloseHandle(hFile);
        outError = "Invalid DOS header signature";
        return false;
    }
    if (SetFilePointer(hFile, dosHdr.e_lfanew, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        CloseHandle(hFile);
        outError = "Invalid NT header offset";
        return false;
    }
    DWORD peSignature = 0;
    if (!ReadFile(hFile, &peSignature, sizeof(peSignature), &read, NULL) || read != sizeof(peSignature) || peSignature != IMAGE_NT_SIGNATURE) {
        CloseHandle(hFile);
        outError = "Invalid PE NT signature";
        return false;
    }
    IMAGE_FILE_HEADER fileHdr = {};
    if (!ReadFile(hFile, &fileHdr, sizeof(fileHdr), &read, NULL) || read != sizeof(fileHdr)) {
        CloseHandle(hFile);
        outError = "Failed to read PE file header";
        return false;
    }
    if (fileHdr.Machine != IMAGE_FILE_MACHINE_AMD64) {
        CloseHandle(hFile);
        outError = "PE machine architecture is not x64 (found 0x" + ToHex((const uint8_t*)&fileHdr.Machine, sizeof(fileHdr.Machine)) + ")";
        return false;
    }
    IMAGE_OPTIONAL_HEADER64 optHdr = {};
    if (!ReadFile(hFile, &optHdr, sizeof(optHdr), &read, NULL) || read != sizeof(optHdr)) {
        CloseHandle(hFile);
        outError = "Failed to read PE optional header";
        return false;
    }
    if (optHdr.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        CloseHandle(hFile);
        outError = "PE optional header magic is not PE32+ (64-bit)";
        return false;
    }
    if (optHdr.Subsystem != IMAGE_SUBSYSTEM_WINDOWS_GUI && optHdr.Subsystem != IMAGE_SUBSYSTEM_WINDOWS_CUI) {
        CloseHandle(hFile);
        outError = "PE subsystem is not Windows GUI or CUI";
        return false;
    }
    CloseHandle(hFile);
    return true;
}

// --- Scoped Temporary Directory Implementation ---

ScopedTempDirectory::ScopedTempDirectory(const std::wstring& prefix) {
    // Portable containment: stage strictly inside the application folder (<root>\cache\updater-tmp\) so no update bytes ever touch %TEMP%
    // or any other directory outside the portable root. The staging tree stays under cache\ so the existing "Clear disposable cache" action
    // can evict orphaned stages from crashed updates. On success the caller disables auto-cleanup and hands the staged directory to the
    // update helper.
    std::wstring root = GetExeDir();
    if (root.empty()) { return; }
    std::wstring baseDir = root + L"\\cache\\updater-tmp";
    SHCreateDirectoryExW(nullptr, GetCacheDir().c_str(), nullptr);
    SHCreateDirectoryExW(nullptr, baseDir.c_str(), nullptr);
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::random_device rd;
    std::mt19937_64 gen(rd() ^ now);
    std::uniform_int_distribution<uint64_t> dis;
    path_ = baseDir + L"\\" + prefix + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(dis(gen));
    if (!IsCachePathSafe(path_, root)) { path_.clear(); return; }
    CreateDirectoryW(path_.c_str(), NULL);
}

ScopedTempDirectory::~ScopedTempDirectory() { Cleanup(); }

ScopedTempDirectory::ScopedTempDirectory(ScopedTempDirectory&& other) noexcept
    : path_(std::move(other.path_)), autoCleanup_(other.autoCleanup_)
{ other.autoCleanup_ = false; }

ScopedTempDirectory& ScopedTempDirectory::operator=(ScopedTempDirectory&& other) noexcept {
    if (this != &other) { Cleanup(); path_ = std::move(other.path_); autoCleanup_ = other.autoCleanup_; other.autoCleanup_ = false; }
    return *this;
}

std::wstring ScopedTempDirectory::SubPath(const std::wstring& rel) const {
    std::wstring p = path_;
    if (!p.empty() && p.back() != L'\\') p += L'\\';
    if (rel.size() >= 2 && rel[0] == L'.' && rel[1] == L'\\') { p += rel.substr(2); } else { p += rel; }
    return p;
}

void ScopedTempDirectory::Cleanup() {
    if (autoCleanup_ && !path_.empty()) {
        try {
            SetFileAttributesW(path_.c_str(), FILE_ATTRIBUTE_NORMAL);
            std::error_code ec;
            for (const auto& entry : fs::recursive_directory_iterator(fs::path(path_), fs::directory_options::skip_permission_denied, ec)) {
                SetFileAttributesW(entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
            }
            fs::remove_all(fs::path(path_), ec);
        } catch (...) {}
        path_.clear();
    }
}

// --- Manifest Validation Implementation ---

UpdateCheckResult ValidateReleaseManifest(const ReleaseManifest &manifest, const UpdaterConfig &config, const std::wstring &rootDir) {
    UpdateCheckResult result;
    result.manifest = manifest;
    // 1. Schema version and Application Name
    if (manifest.schemaVersion != 1) {
        result.status = UpdaterStatusCode::InvalidManifestJson;
        result.message = "Unsupported manifest schemaVersion: " + std::to_string(manifest.schemaVersion);
        return result;
    }
    if (manifest.application != "Aperture") {
        result.status = UpdaterStatusCode::InvalidManifestJson;
        result.message = "Manifest application is not 'Aperture': " + manifest.application;
        return result;
    }
    // 2. Channel policy validation & acceptance
    std::string configChannel = config.channel.empty() ? ChannelPolicy::Instance().GetDefaultChannel() : ChannelPolicy::Instance().CanonicalizeChannel(config.channel);
    std::string manifestChannel = manifest.channel.empty() ? ChannelPolicy::Instance().GetDefaultChannel() : ChannelPolicy::Instance().CanonicalizeChannel(manifest.channel);
    if (!ChannelPolicy::Instance().IsValidChannel(manifest.channel)) {
        result.status = UpdaterStatusCode::InvalidManifestJson;
        result.message = "Unknown or invalid release channel in manifest: " + manifest.channel;
        return result;
    }
    if (!ChannelPolicy::Instance().IsChannelAccepted(configChannel, manifestChannel)) {
        result.status = UpdaterStatusCode::UpToDate;
        result.updateAvailable = false;
        result.message = "Release channel '" + manifestChannel + "' is not eligible for updates on configured channel '" + configChannel + "'";
        return result;
    }
    // 3. Parse Semantic Version & Validate Prerelease Syntax for Channel
    auto manifestVer = SemanticVersion::Parse(manifest.version);
    if (!manifestVer.has_value()) {
        result.status = UpdaterStatusCode::InvalidManifestJson;
        result.message = "Invalid semantic version string: " + manifest.version;
        return result;
    }
    if (manifestChannel == "stable" && !manifestVer->preRelease.empty()) {
        result.status = UpdaterStatusCode::InvalidManifestJson;
        result.message = "Stable release channel rejects prerelease version '" + manifest.version + "'";
        return result;
    }
    if (!manifestVer->preRelease.empty()) {
        if (!ChannelPolicy::Instance().IsValidPrereleaseForChannel(manifestChannel, manifestVer->preRelease)) {
            result.status = UpdaterStatusCode::InvalidManifestJson;
            result.message = "Invalid prerelease format '" + manifestVer->preRelease + "' for release channel '" + manifestChannel + "'";
            return result;
        }
    }
    // 4. Minimum Updater Version requirement
    if (!manifest.minimumUpdaterVersion.empty()) {
        auto minVer = SemanticVersion::Parse(manifest.minimumUpdaterVersion);
        if (minVer.has_value() && SemanticVersion::Current() < *minVer) {
            result.status = UpdaterStatusCode::MinimumUpdaterVersionUnsatisfied;
            result.message = "Current updater version (" + SemanticVersion::Current().ToString() +
                             ") does not satisfy required minimum updater version (" + minVer->ToString() + ")";
            return result;
        }
    }
    // 5. Version comparison (Downgrade & Replay Check)
    SemanticVersion currentVer = SemanticVersion::Current();
    if (!config.allowSameVersion) {
        if (*manifestVer < currentVer) {
            result.status = UpdaterStatusCode::ManifestDowngrade;
            result.updateAvailable = false;
            result.message = "Manifest version (" + manifestVer->ToString() + ") is older than current version (" + currentVer.ToString() + ")";
            return result;
        }
        if (*manifestVer == currentVer) {
            result.status = UpdaterStatusCode::UpToDate;
            result.updateAvailable = false;
            result.message = "Aperture is up to date (" + currentVer.ToString() + ")";
            return result;
        }
    }
    // 6. Timestamps & Expiry Checks
    auto now = config.mockCurrentTime.value_or(std::chrono::system_clock::now());
    std::chrono::system_clock::time_point pubTime, expTime;
    if (!ParseIso8601Utc(manifest.publishedUtc, pubTime)) {
        result.status = UpdaterStatusCode::InvalidManifestJson;
        result.message = "Invalid publishedUtc timestamp: " + manifest.publishedUtc;
        return result;
    }
    if (!ParseIso8601Utc(manifest.expiryUtc, expTime)) {
        result.status = UpdaterStatusCode::InvalidManifestJson;
        result.message = "Invalid expiryUtc timestamp: " + manifest.expiryUtc;
        return result;
    }
    if (expTime <= pubTime) {
        result.status = UpdaterStatusCode::InvalidManifestJson;
        result.message = "expiryUtc must be strictly later than publishedUtc";
        return result;
    }
    // Expiry duration bound check against channel policy
    int maxExpiryDays = ChannelPolicy::Instance().GetChannelMaxExpiryDays(manifestChannel);
    auto maxDuration = std::chrono::hours(24 * (maxExpiryDays + 1));
    if ((expTime - pubTime) > maxDuration) {
        result.status = UpdaterStatusCode::InvalidManifestJson;
        result.message = "Manifest expiry duration exceeds maximum allowed limit (" + std::to_string(maxExpiryDays) + " days) for channel '" + manifestChannel + "'";
        return result;
    }
    // Allow 24 hours clock skew forward for published timestamp
    auto maxSkew = std::chrono::hours(24);
    if (pubTime > (now + maxSkew)) {
        result.status = UpdaterStatusCode::ManifestFutureTimestamp;
        result.message = "Manifest publishedUtc is in the future beyond acceptable clock skew";
        return result;
    }
    if (now > expTime) {
        result.status = UpdaterStatusCode::ManifestExpired;
        result.message = "Release manifest has expired on " + manifest.expiryUtc;
        return result;
    }
    // 7. HTTPS URL & Host Validation
    if (manifest.downloadUrl.rfind("https://", 0) != 0) {
        result.status = UpdaterStatusCode::DisallowedRedirect;
        result.message = "Package downloadUrl must strictly use HTTPS";
        return result;
    }
    URL_COMPONENTS urlComp = { 0 };
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwSchemeLength = (DWORD)-1;
    urlComp.dwHostNameLength = (DWORD)-1;
    std::wstring wUrl = Utf8ToUtf16(manifest.downloadUrl);
    if (!WinHttpCrackUrl(wUrl.c_str(), (DWORD)wUrl.length(), 0, &urlComp) || !urlComp.lpszHostName || urlComp.dwHostNameLength == 0) {
        result.status = UpdaterStatusCode::InvalidManifestJson;
        result.message = "Malformed downloadUrl structure";
        return result;
    }
    std::string downloadHost = Utf16ToUtf8(std::wstring(urlComp.lpszHostName, urlComp.dwHostNameLength));
    if (!config.allowedHosts.empty()) {
        bool hostAllowed = false;
        for (const auto& allowed : config.allowedHosts) {
            if (CaseInsensitiveEqual(downloadHost, allowed)) { hostAllowed = true; break; }
        }
        if (!hostAllowed) {
            result.status = UpdaterStatusCode::DisallowedRedirect;
            result.message = "Download host '" + downloadHost + "' is not in updater allowedHosts allowlist";
            return result;
        }
    }
    // 8. Size Bounds
    if (manifest.fileSizeBytes == 0 || manifest.fileSizeBytes > config.maxPackageSizeBytes) {
        result.status = UpdaterStatusCode::ResponseTooLarge;
        result.message = "Package fileSizeBytes (" + std::to_string(manifest.fileSizeBytes) +
                         ") exceeds maximum package size limit (" + std::to_string(config.maxPackageSizeBytes) + ")";
        return result;
    }
    // 9. SHA-256 Hash format
    if (manifest.sha256.length() != 64) {
        result.status = UpdaterStatusCode::InvalidManifestJson;
        result.message = "Invalid package SHA-256 length: " + manifest.sha256;
        return result;
    }
    // 10. Detached Signature Verification
    if (!config.allowUnsignedForTesting) {
        if (manifest.signature.empty()) {
            result.status = UpdaterStatusCode::InvalidSignature;
            result.message = "Release manifest lacks cryptographic detached signature";
            return result;
        }
        std::string effectivePubKeyHex = config.GetEffectivePublicKeyHex();
        if (effectivePubKeyHex.empty()) {
            result.status = UpdaterStatusCode::NotConfigured;
            result.message = "Pinned release public key is not configured";
            return result;
        }
        std::vector<uint8_t> pubKeyBlob;
        if (!FromHex(effectivePubKeyHex, pubKeyBlob)) {
            result.status = UpdaterStatusCode::NotConfigured;
            result.message = "Invalid format for pinned release public key";
            return result;
        }
        std::string canonicalString = manifest.ComputeCanonicalString();
        if (!VerifyEcdsaP256Signature(canonicalString, manifest.signature, pubKeyBlob)) {
            result.status = UpdaterStatusCode::InvalidSignature;
            result.message = "Cryptographic signature verification failed against pinned release public key";
            return result;
        }
    }
    // 11. Root directory writability & disk space checks
    StorageStatus writability = CheckDataDirectoryWritability();
    if (!writability.is_ok()) {
        result.status = UpdaterStatusCode::UnwritableRoot;
        result.message = "Target root directory is not writable: " + writability.message;
        return result;
    }
    ULARGE_INTEGER freeBytes = {}, totalBytes = {}, totalFree = {};
    if (GetDiskFreeSpaceExW(rootDir.c_str(), &freeBytes, &totalBytes, &totalFree)) {
        if (freeBytes.QuadPart < (manifest.fileSizeBytes * 3)) {
            result.status = UpdaterStatusCode::InsufficientDiskSpace;
            result.message = "Insufficient free disk space for update staging and backup";
            return result;
        }
    }
    result.status = UpdaterStatusCode::UpdateAvailable;
    result.updateAvailable = true;
    result.message = "Update available: " + manifest.version;
    return result;
}

// --- Check for Updates Pipeline ---

UpdateCheckResult CheckForUpdates(const UpdaterConfig &config, IHttpTransport *transport, const std::wstring &rootDir) {
    UpdateCheckResult result;
    const auto cancelled = [&] { if (!config.cancellation || !config.cancellation->IsCancelled()) return false;
        result.status = UpdaterStatusCode::Cancelled; result.message = "Update check cancelled"; return true; };
    if (cancelled()) return result;
    if (!config.IsTrustConfigured()) {
        result.status = UpdaterStatusCode::NotConfigured;
        result.message = "Software update trust root and release endpoints are not configured for this build.";
        return result;
    }
    // Ensure root writability before initiating network check
    StorageStatus writability = CheckDataDirectoryWritability();
    if (!writability.is_ok()) {
        result.status = UpdaterStatusCode::UnwritableRoot;
        result.message = "Aperture executable root is read-only or unwritable: " + writability.message;
        return result;
    }
    std::unique_ptr<IHttpTransport> defaultTransport;
    IHttpTransport* activeTransport = transport;
    if (!activeTransport) { defaultTransport = std::make_unique<WinHttpTransport>(); activeTransport = defaultTransport.get(); }
    HttpRequestOptions options;
    options.maxResponseBytes = config.maxManifestSizeBytes;
    options.timeoutMs = config.connectTimeoutMs;
    options.maxRedirects = config.maxRedirects;
    options.userAgent = L"Aperture/2.0 (Updater)";
    options.cancellation = config.cancellation;
    for (const auto& host : config.allowedHosts) options.allowedHosts.push_back(Utf8ToUtf16(host));
    HttpResponse httpResp = activeTransport->Get(Utf8ToUtf16(config.manifestUrl), options);
    result.httpStatusCode = httpResp.statusCode;
    if (cancelled()) return result;
    if (!httpResp.success) {
        if (httpResp.status == HttpTransportStatus::Timeout) {
            result.status = UpdaterStatusCode::Timeout;
            result.message = "Network timeout while fetching release manifest";
        } else if (httpResp.status == HttpTransportStatus::TooManyRedirects) {
            result.status = UpdaterStatusCode::TooManyRedirects;
            result.message = "Redirect limit exceeded";
        } else if (httpResp.status == HttpTransportStatus::HttpsRequired) {
            result.status = UpdaterStatusCode::DisallowedRedirect;
            result.message = "HTTPS strictly required for release manifest";
        } else if (httpResp.status == HttpTransportStatus::ResponseTooLarge) {
            result.status = UpdaterStatusCode::ResponseTooLarge;
            result.message = "Release manifest response exceeds maximum size limit";
        } else {
            result.status = UpdaterStatusCode::NetworkError;
            result.message = Utf16ToUtf8(httpResp.errorMessage);
            if (result.message.empty()) result.message = "HTTP request failed with status " + std::to_string(httpResp.statusCode);
        }
        return result;
    }
    ReleaseManifest manifest;
    std::string parseErr;
    if (!ReleaseManifest::Parse(httpResp.body, manifest, parseErr)) {
        result.status = UpdaterStatusCode::InvalidManifestJson;
        result.message = "Failed to parse release manifest JSON: " + parseErr;
        return result;
    }
    return ValidateReleaseManifest(manifest, config, rootDir);
}

// --- Download and Stage Update Pipeline ---

UpdateDownloadResult DownloadAndStageUpdate( const ReleaseManifest& manifest, const UpdaterConfig& config, IHttpTransport* transport,
    std::function<void(float progress)> progressCallback, const std::wstring &rootDir) {
    UpdateDownloadResult result;
    result.manifest = manifest;
    const auto cancelled = [&] { if (!config.cancellation || !config.cancellation->IsCancelled()) return false;
        result.status = UpdaterStatusCode::Cancelled; result.message = "Update download cancelled"; return true; };
    if (cancelled()) return result;
    UpdateCheckResult valRes = ValidateReleaseManifest(manifest, config, rootDir);
    if (!valRes.updateAvailable) {
        result.status = valRes.status;
        result.message = valRes.message;
        return result;
    }
    auto tempDir = std::make_shared<ScopedTempDirectory>(L"Aperture_Update_");
    std::wstring stagingDir = tempDir->SubPath(L"staged");
    CreateDirectoryW(stagingDir.c_str(), NULL);
    std::unique_ptr<IHttpTransport> defaultTransport;
    IHttpTransport* activeTransport = transport;
    if (!activeTransport) { defaultTransport = std::make_unique<WinHttpTransport>(); activeTransport = defaultTransport.get(); }
    HttpRequestOptions options;
    options.maxResponseBytes = config.maxPackageSizeBytes;
    options.timeoutMs = config.downloadTimeoutMs;
    options.maxRedirects = config.maxRedirects;
    options.userAgent = L"Aperture/2.0 (Updater)";
    options.cancellation = config.cancellation;
    for (const auto& host : config.allowedHosts) options.allowedHosts.push_back(Utf8ToUtf16(host));
    if (progressCallback) progressCallback(0.1f);
    std::wstring stagedExe = stagingDir + L"\\Aperture.exe";
    HttpResponse httpResp = activeTransport->GetToFile(Utf8ToUtf16(manifest.downloadUrl), stagedExe, options);
    if (cancelled()) return result;
    if (!httpResp.success) {
        result.status = httpResp.status == HttpTransportStatus::FileError ? UpdaterStatusCode::StagingError : UpdaterStatusCode::NetworkError;
        result.message = "Failed to download update package: " + Utf16ToUtf8(httpResp.errorMessage);
        return result;
    }
    if (progressCallback) progressCallback(0.5f);
    std::string downloadedSha = ComputeFileSha256Hex(stagedExe);
    if (cancelled()) return result;
    if (!CaseInsensitiveEqual(downloadedSha, manifest.sha256)) {
        result.status = UpdaterStatusCode::HashMismatch;
        result.message = "Package SHA-256 mismatch: expected " + manifest.sha256 + ", got " + downloadedSha;
        return result;
    }
    result.stagedFiles.push_back(stagedExe);
    std::string peErr;
    if (!VerifyPeArchitectureX64(stagedExe, peErr)) {
        result.status = UpdaterStatusCode::InvalidPeArchitecture;
        result.message = "Downloaded binary architecture verification failed: " + peErr;
        return result;
    }
    std::string expectedSigner = config.GetEffectiveSignerThumbprint();
    if (expectedSigner.empty() && !manifest.signerThumbprint.empty()) { expectedSigner = manifest.signerThumbprint; }
    if (!config.allowUnsignedForTesting) {
        std::string actualThumbprint, authErr;
        if (!VerifyAuthenticodeAndPin(stagedExe, expectedSigner, actualThumbprint, authErr)) {
            result.status = UpdaterStatusCode::AuthenticodeVerificationFailed;
            result.message = "Authenticode verification failed for staged binary: " + authErr;
            return result;
        }
    }
    if (progressCallback) progressCallback(1.0f);
    if (cancelled()) return result;
    result.stagingDirectory = stagingDir;
    result.stagingLease = std::move(tempDir);
    result.status = UpdaterStatusCode::Success;
    result.message = "Update downloaded and staged successfully";
    return result;
}

// --- Helper Mode & Atomic Replacement ---

UpdateApplyResult LaunchHelperAndExit(const UpdateDownloadResult &downloadResult, bool relaunch, const std::wstring &targetDir) {
    UpdateApplyResult result;
    if (downloadResult.status != UpdaterStatusCode::Success || downloadResult.stagingDirectory.empty()) {
        result.status = UpdaterStatusCode::StagingError;
        result.message = "Invalid download result for helper handoff";
        return result;
    }
    std::wstring exePath = GetExePath();
    if (exePath.empty()) {
        result.status = UpdaterStatusCode::HelperValidationError;
        result.message = "Could not resolve current executable path";
        return result;
    }
    DWORD parentPid = GetCurrentProcessId();
    std::wstring cmdLine = L"\"" + exePath + L"\" --update-helper" + L" --parent-pid " + std::to_wstring(parentPid) +
                           L" --target-dir \"" + targetDir + L"\"" + L" --staging-dir \"" + downloadResult.stagingDirectory + L"\"";
    if (relaunch) { cmdLine += L" --relaunch"; }
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');
    BOOL created = CreateProcessW( NULL, cmdBuf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, targetDir.c_str(),
        &si, &pi);
    if (!created) {
        DWORD err = GetLastError();
        result.status = UpdaterStatusCode::HelperValidationError;
        result.message = "Failed to launch update helper process (error " + std::to_string(err) + ")";
        return result;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (downloadResult.stagingLease) downloadResult.stagingLease->DisableAutoCleanup();
    result.status = UpdaterStatusCode::Success;
    result.message = "Update helper launched successfully";
    return result;
}

int RunHelperMode(int argc, wchar_t* argv[]) {
    DWORD parentPid = 0;
    std::wstring targetDir, stagingDir;
    bool relaunch = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--parent-pid" && i + 1 < argc) {
            parentPid = static_cast<DWORD>(std::wcstoul(argv[++i], NULL, 10));
        } else if (arg == L"--target-dir" && i + 1 < argc) { targetDir = argv[++i]; } else if (arg == L"--staging-dir" && i + 1 < argc) {
            stagingDir = argv[++i];
        } else if (arg == L"--relaunch") { relaunch = true; }
    }
    if (parentPid == 0 || targetDir.empty() || stagingDir.empty()) {
        return 101; // Invalid arguments
    }
    targetDir = GetFullPathDynamic(targetDir);
    stagingDir = GetFullPathDynamic(stagingDir);
    HANDLE hParent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, parentPid);
    if (!hParent) {
        return 102; // Cannot open parent process
    }
    DWORD parentSessionId = 0, currentSessionId = 0;
    ProcessIdToSessionId(parentPid, &parentSessionId);
    ProcessIdToSessionId(GetCurrentProcessId(), &currentSessionId);
    if (parentSessionId != currentSessionId) {
        CloseHandle(hParent);
        return 103; // Cross-session elevation attempt rejected
    }
    HANDLE hParentToken = NULL, hCurrentToken = NULL;
    if (OpenProcessToken(hParent, TOKEN_QUERY, &hParentToken) && OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hCurrentToken)) {
        DWORD len1 = 0, len2 = 0;
        GetTokenInformation(hParentToken, TokenUser, NULL, 0, &len1);
        GetTokenInformation(hCurrentToken, TokenUser, NULL, 0, &len2);
        std::vector<uint8_t> buf1(len1), buf2(len2);
        if (GetTokenInformation(hParentToken, TokenUser, buf1.data(), len1, &len1) &&
            GetTokenInformation(hCurrentToken, TokenUser, buf2.data(), len2, &len2)) {
            TOKEN_USER* u1 = reinterpret_cast<TOKEN_USER*>(buf1.data());
            TOKEN_USER* u2 = reinterpret_cast<TOKEN_USER*>(buf2.data());
            if (!EqualSid(u1->User.Sid, u2->User.Sid)) {
                CloseHandle(hParentToken);
                CloseHandle(hCurrentToken);
                CloseHandle(hParent);
                return 104; // User SID mismatch
            }
        }
        CloseHandle(hParentToken);
        CloseHandle(hCurrentToken);
    }
    DWORD waitRes = WaitForSingleObject(hParent, 30000);
    CloseHandle(hParent);
    if (waitRes != WAIT_OBJECT_0) {
        return 105; // Parent process did not exit in time
    }
    if (!DirectoryExists(stagingDir)) {
        return 106; // Staging directory missing
    }
    std::wstring backupDir = targetDir + L"\\_backup_update";
    CreateDirectoryW(backupDir.c_str(), NULL);
    std::vector<std::pair<std::wstring, std::wstring>> swappedFiles;
    bool swapFailed = false;
    try {
        for (const auto& entry : fs::directory_iterator(fs::path(stagingDir))) {
            if (entry.is_regular_file()) {
                std::wstring fileName = entry.path().filename().wstring();
                std::wstring targetPath = targetDir + L"\\" + fileName;
                std::wstring backupPath = backupDir + L"\\" + fileName;
                std::wstring stagedPath = entry.path().wstring();
                if (FileExists(targetPath)) {
                    if (!MoveFileExW(targetPath.c_str(), backupPath.c_str(), MOVEFILE_REPLACE_EXISTING)) { swapFailed = true; break; }
                }
                if (!MoveFileExW(stagedPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    swapFailed = true;
                    break;
                }
                swappedFiles.push_back({ targetPath, backupPath });
            }
        }
    } catch (...) { swapFailed = true; }
    if (swapFailed) {
        for (const auto& pair : swappedFiles) {
            if (FileExists(pair.second)) { MoveFileExW(pair.second.c_str(), pair.first.c_str(), MOVEFILE_REPLACE_EXISTING); }
        }
        try { std::error_code ec; fs::remove_all(fs::path(backupDir), ec); fs::remove_all(fs::path(stagingDir), ec); } catch (...) {}
        return 107; // Swap failed and rolled back
    }
    try { std::error_code ec; fs::remove_all(fs::path(backupDir), ec); fs::remove_all(fs::path(stagingDir), ec); } catch (...) {}
    if (relaunch) {
        std::wstring updatedExe = targetDir + L"\\Aperture.exe";
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        std::wstring cmd = L"\"" + updatedExe + L"\"";
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');
        CreateProcessW( NULL, cmdBuf.data(), NULL, NULL, FALSE, 0, NULL, targetDir.c_str(), &si, &pi);
        if (pi.hThread) CloseHandle(pi.hThread);
        if (pi.hProcess) CloseHandle(pi.hProcess);
    }
    return 0;
}

// --- Durable State Storage Implementation ---

StorageStatus LoadUpdaterState(UpdaterState& outState, const std::wstring& rootDir) {
    outState = UpdaterState();
    std::wstring path = rootDir + L"\\data\\updater.json";
    if (!FileExists(path)) { return StorageStatus::Ok(path); }
    std::string text;
    StorageStatus st = ReadTextFile(path, text);
    if (!st.is_ok()) return st;
    JsonValue root;
    if (!JsonValue::parse(text, root) || !root.is_object())
        return StorageStatus::Error(StorageErrorCode::SerializationFailed, ERROR_INVALID_DATA, path, "Malformed JSON in updater.json");
    outState.lastCheckUtc = root.get("lastCheckUtc").as_string("");
    outState.lastCheckStatus = root.get("lastCheckStatus").as_string("");
    outState.lastAvailableVersion = root.get("lastAvailableVersion").as_string("");
    outState.lastError = root.get("lastError").as_string("");
    return StorageStatus::Ok(path);
}

StorageStatus SaveUpdaterState(const UpdaterState& state, bool isPrivateMode, const std::wstring& rootDir) {
    if (isPrivateMode) { return StorageStatus::Ok(); }
    StorageStatus writability = CheckDataDirectoryWritability();
    if (!writability.is_ok()) return writability;
    JsonValue root = JsonValue::Object();
    root["schemaVersion"] = 1;
    root["lastCheckUtc"] = state.lastCheckUtc;
    root["lastCheckStatus"] = state.lastCheckStatus;
    root["lastAvailableVersion"] = state.lastAvailableVersion;
    root["lastError"] = state.lastError;
    std::wstring path = rootDir + L"\\data\\updater.json";
    return AtomicWriteTextFile(path, root.stringify(true));
}

// --- Status Message Formatting ---

std::wstring GetUpdaterStatusMessage(UpdaterStatusCode status, const std::string& detail) {
    std::wstring msg;
    switch (status) {
        case UpdaterStatusCode::Success: msg = L"Update completed successfully."; break;
        case UpdaterStatusCode::UpToDate: msg = L"Aperture is up to date."; break;
        case UpdaterStatusCode::UpdateAvailable: msg = L"A new version of Aperture is available."; break;
        case UpdaterStatusCode::Disabled: msg = L"Automatic updates are disabled."; break;
        case UpdaterStatusCode::NotConfigured: msg = L"Software update repository is not configured for this build."; break;
        case UpdaterStatusCode::InvalidManifestJson: msg = L"Release manifest contains invalid metadata or JSON structure."; break;
        case UpdaterStatusCode::ManifestExpired: msg = L"Release manifest has expired."; break;
        case UpdaterStatusCode::ManifestFutureTimestamp: msg = L"Release manifest timestamp is in the future."; break;
        case UpdaterStatusCode::ManifestDowngrade: msg = L"Update rejected: manifest version is older than current version."; break;
        case UpdaterStatusCode::ManifestReplay: msg = L"Update rejected: manifest replay detected."; break;
        case UpdaterStatusCode::MinimumUpdaterVersionUnsatisfied: msg = L"This update requires a newer updater component."; break;
        case UpdaterStatusCode::InvalidSignature: msg = L"Cryptographic signature verification failed on release metadata."; break;
        case UpdaterStatusCode::UntrustedSigner: msg = L"Release signature is not signed by a trusted release key."; break;
        case UpdaterStatusCode::HashMismatch: msg = L"Package SHA-256 hash mismatch."; break;
        case UpdaterStatusCode::AuthenticodeVerificationFailed: msg = L"Authenticode code signature verification failed."; break;
        case UpdaterStatusCode::AuthenticodeSignerMismatch: msg = L"Authenticode signer thumbprint does not match pinned trust root."; break;
        case UpdaterStatusCode::InvalidPeArchitecture: msg = L"Downloaded binary architecture is not compatible with Windows x64."; break;
        case UpdaterStatusCode::NetworkError: msg = L"Network error during update fetch."; break;
        case UpdaterStatusCode::Timeout: msg = L"Network connection timed out."; break;
        case UpdaterStatusCode::TooManyRedirects: msg = L"Too many network redirects."; break;
        case UpdaterStatusCode::DisallowedRedirect: msg = L"Insecure or disallowed redirect destination."; break;
        case UpdaterStatusCode::HttpError: msg = L"HTTP request failed."; break;
        case UpdaterStatusCode::ResponseTooLarge: msg = L"Download size exceeded maximum allowed limit."; break;
        case UpdaterStatusCode::UnwritableRoot: msg = L"Aperture directory is read-only or unwritable."; break;
        case UpdaterStatusCode::InsufficientDiskSpace: msg = L"Insufficient free disk space for update."; break;
        case UpdaterStatusCode::StagingError: msg = L"Error staging update files."; break;
        case UpdaterStatusCode::HelperValidationError: msg = L"Update helper process validation failed."; break;
        case UpdaterStatusCode::SwapFailed: msg = L"File replacement failed."; break;
        case UpdaterStatusCode::RollbackSucceeded: msg = L"Update failed and was cleanly rolled back."; break;
        case UpdaterStatusCode::RollbackFailed: msg = L"Critical error: update failed and rollback was incomplete."; break;
        case UpdaterStatusCode::Cancelled: msg = L"Update was cancelled."; break;
    }
    if (!detail.empty()) { msg += L" (" + Utf8ToUtf16(detail) + L")"; }
    return msg;
}

} // namespace Aperture
