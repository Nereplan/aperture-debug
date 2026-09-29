#include "../support/TestFramework.h"
#include "../../src/Common.h"
#include "../../src/Json.h"
#include "../../src/PlaylistIO.h"
#include "../../src/Subtitles.h"
#include "../../src/SubtitleDiscovery.h"
#include "../../src/NetworkStream.h"
#include "../../src/IpcProtocol.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/ChannelPolicy.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <bcrypt.h>

#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <chrono>
#include <algorithm>
#include <filesystem>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")

namespace fs = std::filesystem;
using namespace Aperture;

namespace {

std::string ComputeSha256(const void* data, size_t size) {
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (status != 0) return "";
    DWORD hashLen = 32;
    status = BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0);
    if (status != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return "";
    }
    BCryptHashData(hHash, (PUCHAR)data, (ULONG)size, 0);
    std::vector<uint8_t> hashBuf(hashLen);
    BCryptFinishHash(hHash, hashBuf.data(), hashLen, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    std::ostringstream ss;
    for (uint8_t b : hashBuf) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    }
    return ss.str();
}

bool ReadBinaryFile(const std::wstring& path, std::vector<uint8_t>& outData) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart < 0 || size.QuadPart > 100 * 1024 * 1024) {
        CloseHandle(hFile);
        return false;
    }
    outData.resize(static_cast<size_t>(size.QuadPart));
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, outData.data(), static_cast<DWORD>(outData.size()), &bytesRead, NULL);
    CloseHandle(hFile);
    return ok && (bytesRead == outData.size());
}

std::wstring ToWString(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    if (len <= 0) return L"";
    std::wstring result(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &result[0], len);
    return result;
}

std::string ToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
    if (len <= 0) return "";
    std::string result(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &result[0], len, NULL, NULL);
    return result;
}

struct ManifestFixture {
    std::string id;
    std::string category;
    std::string path;
    std::string sha256;
    size_t sizeBytes = 0;
    std::string expectedOutcome; // "accept" or "reject"
    std::string expectedReason;
    std::string format;
    JsonValue expectedFields;
};

struct ManifestData {
    int version = 0;
    std::string description;
    std::vector<std::string> categories;
    std::vector<ManifestFixture> fixtures;
};

bool LoadManifest(const std::wstring& manifestPath, ManifestData& outData, std::string& outError) {
    std::string rawContent;
    StorageStatus st = ReadTextFile(manifestPath, rawContent);
    if (!st.is_ok()) {
        outError = "Could not read manifest file at: " + ToUtf8(manifestPath);
        return false;
    }

    JsonValue root;
    if (!JsonValue::parse(rawContent, root) || !root.is_object()) {
        outError = "Failed to parse manifest JSON.";
        return false;
    }

    outData.version = root.get("version").as_int(0);
    outData.description = root.get("description").as_string();

    const auto& catArr = root.get("categories").as_array();
    for (const auto& c : catArr) {
        outData.categories.push_back(c.as_string());
    }

    const auto& fixArr = root.get("fixtures").as_array();
    if (fixArr.empty()) {
        outError = "Manifest contains no fixtures.";
        return false;
    }

    for (const auto& f : fixArr) {
        ManifestFixture mf;
        mf.id = f.get("id").as_string();
        mf.category = f.get("category").as_string();
        mf.path = f.get("path").as_string();
        mf.sha256 = f.get("sha256").as_string();
        mf.sizeBytes = static_cast<size_t>(f.get("sizeBytes").as_int64(0));
        mf.expectedOutcome = f.get("expectedOutcome").as_string();
        mf.expectedReason = f.get("expectedReason").as_string();
        mf.format = f.get("format").as_string();
        if (f.has_key("expectedFields")) {
            mf.expectedFields = f.get("expectedFields");
        }
        outData.fixtures.push_back(mf);
    }

    return true;
}

std::wstring FindManifestPath(const std::wstring& overridePath) {
    if (!overridePath.empty() && FileExists(overridePath)) {
        return overridePath;
    }

    std::vector<std::wstring> candidates = {
        overridePath,
        L"tests\\corpus\\manifest.json",
        L"..\\tests\\corpus\\manifest.json",
        L"..\\..\\tests\\corpus\\manifest.json",
        GetExeDir() + L"\\manifest.json",
        GetExeDir() + L"\\tests\\corpus\\manifest.json",
        GetExeDir() + L"\\..\\..\\tests\\corpus\\manifest.json"
    };

    for (const auto& cand : candidates) {
        if (!cand.empty() && FileExists(cand)) {
            wchar_t fullPath[MAX_PATH];
            if (GetFullPathNameW(cand.c_str(), MAX_PATH, fullPath, NULL)) {
                return std::wstring(fullPath);
            }
            return cand;
        }
    }
    return L"";
}

} // namespace

int main(int argc, char* argv[]) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    std::wstring manifestArg = L"";
    std::string categoryFilter = "";
    std::string idFilter = "";
    bool listMode = false;
    bool validateOnly = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--manifest" || arg == "-m") && i + 1 < argc) {
            manifestArg = ToWString(argv[++i]);
        } else if ((arg == "--category" || arg == "-c") && i + 1 < argc) {
            categoryFilter = argv[++i];
        } else if ((arg == "--id") && i + 1 < argc) {
            idFilter = argv[++i];
        } else if (arg == "--list" || arg == "-l") {
            listMode = true;
        } else if (arg == "--validate" || arg == "-v") {
            validateOnly = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: Aperture_Corpus_Tests.exe [options]\n"
                      << "  --manifest <path>  Path to manifest.json\n"
                      << "  --category <name>  Filter by category (json, playlist, subtitle, url, command_line, settings, media)\n"
                      << "  --id <id>          Filter by exact fixture ID\n"
                      << "  --list             List all registered fixtures\n"
                      << "  --validate         Validate manifest schema, file hashes, and sizes without parser execution\n"
                      << "  --help, -h         Show help\n";
            MFShutdown();
            CoUninitialize();
            return 0;
        }
    }

    std::wstring manifestPath = FindManifestPath(manifestArg);
    if (manifestPath.empty()) {
        std::cerr << "[ERROR] manifest.json could not be located. Pass --manifest <path>.\n";
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    std::wstring corpusRoot = fs::path(manifestPath).parent_path().wstring();

    ManifestData manifest;
    std::string loadErr;
    if (!LoadManifest(manifestPath, manifest, loadErr)) {
        std::cerr << "[ERROR] " << loadErr << "\n";
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    auto SetColor = [hConsole](WORD color) {
        if (hConsole && hConsole != INVALID_HANDLE_VALUE) {
            SetConsoleTextAttribute(hConsole, color);
        }
    };

    if (listMode) {
        std::cout << "Aperture Corpus Inventory (" << manifest.fixtures.size() << " fixtures):\n";
        std::cout << std::left << std::setw(38) << "ID"
                  << std::setw(15) << "Category"
                  << std::setw(10) << "Outcome"
                  << std::setw(10) << "Size(B)"
                  << "Path\n";
        std::cout << std::string(100, '-') << "\n";
        for (const auto& fix : manifest.fixtures) {
            std::cout << std::left << std::setw(38) << fix.id
                      << std::setw(15) << fix.category
                      << std::setw(10) << fix.expectedOutcome
                      << std::setw(10) << fix.sizeBytes
                      << fix.path << "\n";
        }
        MFShutdown();
        CoUninitialize();
        return 0;
    }

    // Hash and integrity check on all fixtures
    std::vector<std::string> integrityErrors;
    std::set<std::string> seenIds;
    std::set<std::string> registeredPaths;

    for (const auto& fix : manifest.fixtures) {
        if (fix.id.empty()) {
            integrityErrors.push_back("Fixture missing ID.");
            continue;
        }
        if (!seenIds.insert(fix.id).second) {
            integrityErrors.push_back("Duplicate fixture ID: " + fix.id);
        }

        std::wstring fullPath = corpusRoot + L"\\" + ToWString(fix.path);
        registeredPaths.insert(fs::path(fullPath).lexically_normal().string());

        if (!FileExists(fullPath)) {
            integrityErrors.push_back("Fixture file not found: " + fix.path);
            continue;
        }

        std::vector<uint8_t> data;
        if (!ReadBinaryFile(fullPath, data)) {
            integrityErrors.push_back("Failed to read fixture: " + fix.path);
            continue;
        }

        if (data.size() != fix.sizeBytes) {
            integrityErrors.push_back("Fixture size mismatch for '" + fix.id + "': expected " +
                                     std::to_string(fix.sizeBytes) + ", got " + std::to_string(data.size()));
        }

        std::string actualHash = ComputeSha256(data.data(), data.size());
        if (actualHash != fix.sha256) {
            integrityErrors.push_back("Fixture SHA-256 mismatch for '" + fix.id + "': expected " +
                                     fix.sha256 + ", got " + actualHash);
        }
    }

    // Check for orphan files in fixtures/
    std::wstring fixturesDir = corpusRoot + L"\\fixtures";
    if (fs::exists(fixturesDir)) {
        for (const auto& entry : fs::recursive_directory_iterator(fixturesDir)) {
            if (entry.is_regular_file()) {
                std::string normPath = entry.path().lexically_normal().string();
                if (registeredPaths.find(normPath) == registeredPaths.end()) {
                    integrityErrors.push_back("Orphan fixture file on disk not registered in manifest: " + normPath);
                }
            }
        }
    }

    if (!integrityErrors.empty()) {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "========================================================\n";
        std::cerr << "  CORPUS INTEGRITY VALIDATION FAILED (" << integrityErrors.size() << " errors)\n";
        std::cerr << "========================================================\n";
        for (const auto& err : integrityErrors) {
            std::cerr << "  - " << err << "\n";
        }
        SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    if (validateOnly) {
        SetColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        std::cout << "[PASS] Corpus manifest validation successful ("
                  << manifest.fixtures.size() << " fixtures verified, 0 errors, 0 drift).\n";
        SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        MFShutdown();
        CoUninitialize();
        return 0;
    }

    SetColor(FOREGROUND_INTENSITY | FOREGROUND_BLUE | FOREGROUND_GREEN);
    std::cout << "========================================================\n";
    std::cout << "  Aperture Native - Parser & Media Corpus Test Runner   \n";
    std::cout << "========================================================\n";
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    struct CategoryStats {
        size_t total = 0;
        size_t passed = 0;
        size_t failed = 0;
    };

    std::map<std::string, CategoryStats> catStats;
    std::set<std::string> consumedFixtures;
    std::vector<std::string> failureDetails;

    auto startTime = std::chrono::high_resolution_clock::now();

    for (const auto& fix : manifest.fixtures) {
        if (!categoryFilter.empty() && fix.category != categoryFilter) {
            continue;
        }
        if (!idFilter.empty() && fix.id != idFilter) {
            continue;
        }

        catStats[fix.category].total++;

        // Ensure each fixture is consumed exactly once
        if (!consumedFixtures.insert(fix.id).second) {
            std::string failMsg = "Fixture " + fix.id + " consumed more than once!";
            failureDetails.push_back(failMsg);
            catStats[fix.category].failed++;
            continue;
        }

        std::wstring fullPath = corpusRoot + L"\\" + ToWString(fix.path);
        std::vector<uint8_t> rawBytes;
        if (!ReadBinaryFile(fullPath, rawBytes)) {
            failureDetails.push_back(fix.id + ": Failed to read fixture file " + fix.path);
            catStats[fix.category].failed++;
            continue;
        }

        std::string utf8Content(rawBytes.begin(), rawBytes.end());
        bool testPassed = false;
        std::string failureReason = "";

        try {
            if (fix.category == "json") {
                JsonValue parsed;
                bool ok = JsonValue::parse(utf8Content, parsed);
                if (fix.expectedOutcome == "accept") {
                    if (!ok || parsed.is_null()) {
                        failureReason = "JsonValue::parse failed on valid JSON.";
                    } else {
                        testPassed = true;
                        if (fix.expectedFields.has_key("type")) {
                            std::string expectedType = fix.expectedFields.get("type").as_string();
                            if (expectedType == "object" && !parsed.is_object()) testPassed = false;
                            if (expectedType == "array" && !parsed.is_array()) testPassed = false;
                        }
                        if (fix.expectedFields.has_key("hasKeys")) {
                            for (const auto& k : fix.expectedFields.get("hasKeys").as_array()) {
                                if (!parsed.has_key(k.as_string())) {
                                    testPassed = false;
                                    failureReason = "Missing expected key: " + k.as_string();
                                }
                            }
                        }
                        if (fix.expectedFields.has_key("appName")) {
                            if (parsed.get("app").as_string() != fix.expectedFields.get("appName").as_string()) {
                                testPassed = false;
                                failureReason = "App name mismatch";
                            }
                        }
                    }
                } else {
                    if (!ok && parsed.is_null()) {
                        testPassed = true;
                    } else {
                        failureReason = "Malformed JSON was unexpectedly accepted.";
                    }
                }
            } else if (fix.category == "playlist") {
                std::wstring playlistDir = fs::path(fullPath).parent_path().wstring();
                PlaylistParseOptions opts;
                opts.allowEmptyPlaylist = false;
                opts.checkFileExistence = false; // deterministic testing without local file dependencies

                PlaylistFormat detected = PlaylistIO::DetectFormatFromContent(utf8Content);
                PlaylistParseResult res = PlaylistIO::ParseContent(utf8Content, PlaylistFormat::Auto, playlistDir, opts);

                if (fix.id == "playlist_malformed_no_header_pls") {
                    PlaylistParseResult plsRes = PlaylistIO::ParsePLSContent(utf8Content, playlistDir, opts);
                    if (!plsRes.is_ok() && plsRes.errorCode == PlaylistIoErrorCode::InvalidHeader) {
                        testPassed = true;
                    } else {
                        failureReason = "PLS missing header was not rejected with InvalidHeader.";
                    }
                } else if (fix.id == "playlist_empty_rejected") {
                    PlaylistParseResult emptyRes = PlaylistIO::ParseM3UContent("", playlistDir, opts);
                    if (!emptyRes.is_ok() && emptyRes.errorCode == PlaylistIoErrorCode::EmptyPlaylist) {
                        testPassed = true;
                    } else {
                        failureReason = "Empty playlist was not rejected with EmptyPlaylist.";
                    }
                } else if (fix.expectedOutcome == "accept") {
                    if (res.is_ok() && !res.entries.empty()) {
                        testPassed = true;
                        if (fix.expectedFields.has_key("entryCount")) {
                            size_t expCount = static_cast<size_t>(fix.expectedFields.get("entryCount").as_int(0));
                            if (res.entries.size() != expCount) {
                                testPassed = false;
                                failureReason = "Playlist entry count mismatch: expected " + std::to_string(expCount) + ", got " + std::to_string(res.entries.size());
                            }
                        }
                        if (fix.expectedFields.has_key("firstTitle") && !res.entries.empty()) {
                            std::wstring expTitle = ToWString(fix.expectedFields.get("firstTitle").as_string());
                            if (res.entries[0].title != expTitle) {
                                testPassed = false;
                                failureReason = "First entry title mismatch: expected '" + ToUtf8(expTitle) + "', got '" + ToUtf8(res.entries[0].title) + "'";
                            }
                        }
                        if (fix.expectedFields.has_key("firstDuration") && !res.entries.empty()) {
                            double expDur = fix.expectedFields.get("firstDuration").as_number();
                            if (std::abs(res.entries[0].durationSec - expDur) > 0.001) {
                                testPassed = false;
                                failureReason = "First entry duration mismatch";
                            }
                        }
                        if (fix.expectedFields.has_key("secondIsUrl") && res.entries.size() > 1) {
                            if (!res.entries[1].isUrl) {
                                testPassed = false;
                                failureReason = "Second entry was expected to be a URL";
                            }
                        }
                    } else {
                        failureReason = "Valid playlist failed to parse: " + res.message;
                    }
                } else {
                    if (!res.is_ok() || res.entries.empty()) {
                        testPassed = true;
                    } else {
                        failureReason = "Malformed playlist was unexpectedly accepted.";
                    }
                }
            } else if (fix.category == "subtitle") {
                std::string decoded = SubtitleEngine::DecodeSubtitleBytes(utf8Content);
                std::vector<SubtitleCue> cues;
                bool ok = false;
                if (fix.format == "srt" || fix.path.find(".srt") != std::string::npos) {
                    ok = SubtitleEngine::ParseSrt(decoded, cues);
                } else if (fix.format == "vtt" || fix.path.find(".vtt") != std::string::npos) {
                    ok = SubtitleEngine::ParseVtt(decoded, cues);
                } else if (fix.format == "ass" || fix.path.find(".ass") != std::string::npos) {
                    ok = SubtitleEngine::ParseAss(decoded, cues);
                }

                if (fix.expectedOutcome == "accept") {
                    if (ok && !cues.empty()) {
                        testPassed = true;
                        if (fix.expectedFields.has_key("cueCount")) {
                            size_t expCount = static_cast<size_t>(fix.expectedFields.get("cueCount").as_int(0));
                            if (cues.size() != expCount) {
                                testPassed = false;
                                failureReason = "Cue count mismatch: expected " + std::to_string(expCount) + ", got " + std::to_string(cues.size());
                            }
                        }
                        if (fix.expectedFields.has_key("firstStartSec") && !cues.empty()) {
                            double expStart = fix.expectedFields.get("firstStartSec").as_number();
                            if (std::abs(cues[0].startTimeSec - expStart) > 0.01) {
                                testPassed = false;
                                failureReason = "First cue start time mismatch";
                            }
                        }
                    } else {
                        failureReason = "Valid subtitle failed to parse cues.";
                    }
                } else {
                    if (!ok || cues.empty()) {
                        testPassed = true;
                    } else {
                        failureReason = "Malformed subtitle unexpectedly produced valid cues.";
                    }
                }
            } else if (fix.category == "url") {
                std::wstring wUrl = ToWString(utf8Content);
                while (!wUrl.empty() && (wUrl.back() == L'\r' || wUrl.back() == L'\n' || wUrl.back() == L' ')) {
                    wUrl.pop_back();
                }
                while (!wUrl.empty() && (wUrl.front() == L'\r' || wUrl.front() == L'\n' || wUrl.front() == L' ')) {
                    wUrl.erase(wUrl.begin());
                }

                if (fix.expectedFields.has_key("hasUrl")) {
                    DroppedUrlResult dropRes = NetworkStreamValidator::ExtractAndValidateDroppedText(wUrl);
                    if (dropRes.hasUrl && dropRes.parsedUrl.isValid) {
                        testPassed = true;
                    } else {
                        failureReason = "Candidate URL extraction failed.";
                    }
                } else {
                    NetworkStreamUrl parsed = NetworkStreamValidator::ValidateAndParse(wUrl);
                    if (fix.expectedOutcome == "accept") {
                        if (parsed.isValid) {
                            testPassed = true;
                            if (fix.expectedFields.has_key("scheme")) {
                                std::string expScheme = fix.expectedFields.get("scheme").as_string();
                                if (expScheme == "Https" && parsed.scheme != NetworkStreamScheme::Https) testPassed = false;
                                if (expScheme == "Http" && parsed.scheme != NetworkStreamScheme::Http) testPassed = false;
                                if (expScheme == "Rtsp" && parsed.scheme != NetworkStreamScheme::Rtsp) testPassed = false;
                                if (expScheme == "Rtmp" && parsed.scheme != NetworkStreamScheme::Rtmp) testPassed = false;
                            }
                            if (fix.expectedFields.has_key("port")) {
                                int expPort = fix.expectedFields.get("port").as_int(0);
                                if (parsed.port != expPort) {
                                    testPassed = false;
                                    failureReason = "Port mismatch: expected " + std::to_string(expPort) + ", got " + std::to_string(parsed.port);
                                }
                            }
                        } else {
                            failureReason = "Valid URL rejected: " + ToUtf8(NetworkStreamValidator::GetSafeErrorMessage(parsed.error));
                        }
                    } else {
                        if (!parsed.isValid) {
                            testPassed = true;
                            if (fix.expectedFields.has_key("error")) {
                                std::string expErr = fix.expectedFields.get("error").as_string();
                                if (expErr == "UnsupportedScheme" && parsed.error != NetworkStreamValidationError::UnsupportedScheme) testPassed = false;
                                if (expErr == "MissingSchemeDelimiter" && parsed.error != NetworkStreamValidationError::MissingSchemeDelimiter) testPassed = false;
                                if (expErr == "EmbeddedCredentialsNotAllowed" && parsed.error != NetworkStreamValidationError::EmbeddedCredentialsNotAllowed) testPassed = false;
                                if (expErr == "InvalidPort" && parsed.error != NetworkStreamValidationError::InvalidPort) testPassed = false;
                                if (expErr == "ContainsControlCharacters" && parsed.error != NetworkStreamValidationError::ContainsControlCharacters) testPassed = false;
                                if (expErr == "InvalidHost" && parsed.error != NetworkStreamValidationError::InvalidHost) testPassed = false;
                            }
                        } else {
                            failureReason = "Invalid URL was unexpectedly accepted as valid.";
                        }
                    }
                }
            } else if (fix.category == "command_line") {
                if (fix.format == "ipc") {
                    PlaybackCommandLineOptions opts;
                    Ipc::ValidationResult vRes = Ipc::DeserializeEnvelope(rawBytes.data(), rawBytes.size(), opts, nullptr, false);
                    if (fix.expectedOutcome == "accept") {
                        if (vRes == Ipc::ValidationResult::Success) {
                            testPassed = true;
                            if (fix.expectedFields.has_key("startPositionSec")) {
                                double expStart = fix.expectedFields.get("startPositionSec").as_number();
                                if (!opts.startPositionSec.has_value() || std::abs(opts.startPositionSec.value() - expStart) > 0.001) {
                                    testPassed = false;
                                    failureReason = "Start position mismatch in IPC envelope";
                                }
                            }
                            if (fix.expectedFields.has_key("volume")) {
                                double expVol = fix.expectedFields.get("volume").as_number();
                                if (!opts.volume.has_value() || std::abs(opts.volume.value() - expVol) > 0.001) {
                                    testPassed = false;
                                    failureReason = "Volume mismatch in IPC envelope";
                                }
                            }
                            if (fix.expectedFields.has_key("targetCount")) {
                                size_t expTargets = static_cast<size_t>(fix.expectedFields.get("targetCount").as_int(0));
                                if (opts.mediaTargets.size() != expTargets) {
                                    testPassed = false;
                                    failureReason = "Media targets count mismatch in IPC envelope";
                                }
                            }
                        } else {
                            failureReason = "IPC envelope deserialization failed with code: " + std::to_string(static_cast<int>(vRes));
                        }
                    } else {
                        if (vRes != Ipc::ValidationResult::Success) {
                            testPassed = true;
                        } else {
                            failureReason = "Corrupt/truncated IPC envelope was unexpectedly accepted.";
                        }
                    }
                } else {
                    std::wstring cmdLine = ToWString(utf8Content);
                    CommandLineParseOutput out = Ipc::ParseCommandLine(cmdLine);
                    if (fix.expectedOutcome == "accept") {
                        if (out.IsSuccess()) {
                            testPassed = true;
                            if (fix.expectedFields.has_key("startPositionSec")) {
                                double expStart = fix.expectedFields.get("startPositionSec").as_number();
                                if (!out.options.startPositionSec.has_value() || std::abs(out.options.startPositionSec.value() - expStart) > 0.001) {
                                    testPassed = false;
                                    failureReason = "Start position mismatch in CLI flags";
                                }
                            }
                            if (fix.expectedFields.has_key("volume")) {
                                double expVol = fix.expectedFields.get("volume").as_number();
                                if (!out.options.volume.has_value() || std::abs(out.options.volume.value() - expVol) > 0.001) {
                                    testPassed = false;
                                    failureReason = "Volume mismatch in CLI flags";
                                }
                            }
                            if (fix.expectedFields.has_key("targetCount")) {
                                size_t expTargets = static_cast<size_t>(fix.expectedFields.get("targetCount").as_int(0));
                                if (out.options.mediaTargets.size() != expTargets) {
                                    testPassed = false;
                                    failureReason = "Media targets count mismatch in CLI";
                                }
                            }
                        } else {
                            failureReason = "Valid CLI flags failed to parse: " + ToUtf8(out.errorMessage);
                        }
                    } else {
                        if (!out.IsSuccess()) {
                            testPassed = true;
                        } else {
                            failureReason = "Malformed CLI flags were unexpectedly accepted.";
                        }
                    }
                }
            } else if (fix.category == "settings") {
                if (fix.id == "settings_valid_channel_policy") {
                    ChannelPolicy policy;
                    std::string policyErr;
                    if (policy.LoadFromJson(utf8Content, policyErr) && policy.GetChannels().size() == 3) {
                        testPassed = true;
                    } else {
                        failureReason = "Channel policy JSON failed to load: " + policyErr;
                    }
                } else {
                    JsonValue root;
                    bool jsonOk = JsonValue::parse(utf8Content, root);
                    if (fix.expectedOutcome == "accept") {
                        if (jsonOk && root.is_object() && root.has_key("themePreset")) {
                            testPassed = true;
                        } else {
                            failureReason = "Settings JSON failed to parse.";
                        }
                    } else {
                        if (!jsonOk || !root.is_object() || root.get("version").as_int(-1) == -1) {
                            testPassed = true;
                        } else {
                            failureReason = "Malformed settings were unexpectedly accepted.";
                        }
                    }
                }
            } else if (fix.category == "media") {
                double durationSec = 0.0;
                uint32_t width = 0;
                uint32_t height = 0;
                bool probed = MediaLibrary::ProbeMediaMetadata(fullPath, durationSec, width, height);

                if (fix.expectedOutcome == "accept") {
                    if (probed) {
                        testPassed = true;
                        if (fix.expectedFields.has_key("width")) {
                            uint32_t expW = static_cast<uint32_t>(fix.expectedFields.get("width").as_int(0));
                            if (width != expW) {
                                testPassed = false;
                                failureReason = "Media probed width mismatch: expected " + std::to_string(expW) + ", got " + std::to_string(width);
                            }
                        }
                    } else {
                        failureReason = "MediaFoundation metadata probe failed on valid media container.";
                    }
                } else {
                    if (!probed) {
                        testPassed = true;
                    } else {
                        failureReason = "Corrupt/invalid media container was unexpectedly probed successfully.";
                    }
                }
            }
        } catch (const std::exception& ex) {
            testPassed = false;
            failureReason = std::string("Exception thrown: ") + ex.what();
        } catch (...) {
            testPassed = false;
            failureReason = "Unhandled non-standard exception thrown.";
        }

        if (testPassed) {
            catStats[fix.category].passed++;
            SetColor(FOREGROUND_GREEN);
            std::cout << "[ PASS ] " << std::left << std::setw(42) << fix.id
                      << " (" << fix.category << ")\n";
            SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        } else {
            catStats[fix.category].failed++;
            SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
            std::cout << "[ FAIL ] " << std::left << std::setw(42) << fix.id
                      << " (" << fix.category << ") -> " << failureReason << "\n";
            SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            failureDetails.push_back(fix.id + ": " + failureReason);
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    double elapsedMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

    std::cout << "\n========================================================\n";
    std::cout << "                 CORPUS TEST SUMMARY                    \n";
    std::cout << "========================================================\n";

    size_t totalAll = 0;
    size_t passedAll = 0;
    size_t failedAll = 0;

    for (const auto& [cat, stats] : catStats) {
        totalAll += stats.total;
        passedAll += stats.passed;
        failedAll += stats.failed;

        std::cout << "  Category: " << std::left << std::setw(15) << cat
                  << " Total: " << std::setw(4) << stats.total
                  << " Passed: " << std::setw(4) << stats.passed
                  << " Failed: " << std::setw(4) << stats.failed << "\n";
    }

    std::cout << "--------------------------------------------------------\n";
    std::cout << "  Total Fixtures Executed: " << consumedFixtures.size()
              << " (" << std::fixed << std::setprecision(2) << elapsedMs << " ms)\n";

    if (failedAll == 0 && failureDetails.empty()) {
        SetColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        std::cout << "  ALL " << passedAll << " CORPUS FIXTURES PASSED OBSERVED ACCEPT/REJECT BEHAVIOR\n";
        SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        std::cout << "========================================================\n";
        MFShutdown();
        CoUninitialize();
        return 0;
    } else {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cout << "  CORPUS TESTS FAILED: " << failedAll << " failed fixtures\n";
        for (const auto& f : failureDetails) {
            std::cout << "    - " << f << "\n";
        }
        SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        std::cout << "========================================================\n";
        MFShutdown();
        CoUninitialize();
        return 1;
    }
}
