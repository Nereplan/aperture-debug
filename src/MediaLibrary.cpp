#include "MediaProbeIsolation.h"
#include "MediaLibrary.h"
#include "WatchReconcile.h"
#include "FolderRediscovery.h"
#include "LibrarySaveComparison.h"
#include "SmartRuleParsing.h"
#include "PreparedSmartPlaylist.h"
#include "PlaybackTarget.h"
#include "LibraryQuery.h"
#include "AudioConfig.h"
#include "MediaFoundation.h"
#include <fstream>
#include <sstream>
#include <charconv>
#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <set>
#include <string_view>
#include <unordered_set>
#include <windows.h>

namespace Aperture {
namespace {

constexpr ULONGLONG kSmartAvailabilityCacheTtlMs = 2000;

// Keep the supported suffixes in one place. This is also the lookup order
// used by the allocation-free membership test below.
constexpr std::array<std::wstring_view, 37> kSupportedExtensionValues = {
    L".3g2", L".3gp", L".aac", L".aiff", L".alac", L".asf", L".avi", L".divx", L".f4v", L".flac",
    L".flv", L".m2ts", L".m2v", L".m4a", L".m4p", L".m4v", L".mka", L".mkv", L".mov", L".mp3",
    L".mp4", L".mpe", L".mpeg", L".mpg", L".mpv", L".ogg", L".ogv", L".opus", L".pcm", L".rm",
    L".rmvb", L".ts", L".vob", L".wav", L".webm", L".wma", L".wmv"
};

std::wstring Identity(const std::wstring& input) {
    if (input.empty()) return {};
    std::wstring s = ResolveAppPath(input);
    for (auto& c : s) { if (c == L'/') c = L'\\'; c = (wchar_t)towlower(c); }
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

bool SamePath(const std::wstring& a, const std::wstring& b) { return Identity(a) == Identity(b); }
inline std::wstring NormalizeDirectoryPath(const std::wstring& path) {
    if (path.empty()) return L"";
    std::wstring result = path;
    for (auto& c : result) { if (c == L'/') c = L'\\'; c = (wchar_t)towlower(c); }
    while (result.size() > 1 && (result.back() == L'\\' || result.back() == L'/')) { result.pop_back(); }
    return result;
}

bool IsReparsePoint(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

std::wstring_view PathExtension(std::wstring_view path) {
    // A Windows UNC server is a root name, so a server-only path has no
    // filename even when its server component contains dots.
    if (path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') &&
        (path[1] == L'\\' || path[1] == L'/')) {
        size_t serverEnd = path.find_first_of(L"/\\", 2);
        if (serverEnd == std::wstring_view::npos) return {};
        path.remove_prefix(serverEnd);
    } else if (path.size() >= 2 &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':') {
        // A drive designator is a root name. In a drive-relative path such as
        // "C:.mp4", the filename is ".mp4" and has no extension.
        path.remove_prefix(2);
    }
    const size_t separator = path.find_last_of(L"/\\");
    if (separator != std::wstring_view::npos) path.remove_prefix(separator + 1);
    if (path.empty() || path == L"." || path == L"..") return {};

    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring_view::npos) return {};
    // std::filesystem::path treats a lone leading-dot filename (".name") as
    // having no extension, while a later dot starts the extension.
    if (dot == 0 && path.find(L'.', 1) == std::wstring_view::npos) return {};
    return path.substr(dot);
}

std::wstring LowerExtension(const std::wstring& path) {
    std::wstring ext = std::filesystem::path(path).extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

bool IsSupportedExtensionValue(std::wstring_view ext) {
    // Comparing a view avoids allocating filesystem::path and extension
    // strings for every filesystem entry visited during a scan.
    if (ext.size() > 5) return false;
    wchar_t lowered[5]{};
    for (size_t i = 0; i < ext.size(); ++i) lowered[i] = static_cast<wchar_t>(std::towlower(ext[i]));
    const std::wstring_view normalized(lowered, ext.size());
    const auto found = std::lower_bound(kSupportedExtensionValues.begin(), kSupportedExtensionValues.end(), normalized);
    return found != kSupportedExtensionValues.end() && *found == normalized;
}

double SafeNumber(const JsonValue& v, double d = 0) { double n = v.as_number(d); return std::isfinite(n) ? n : d; }

int64_t SafeTime(const JsonValue& v) { double n = SafeNumber(v, 0); return n > 0 && n < (double)INT64_MAX ? (int64_t)n : 0; }

uint64_t ExactUint64(const JsonValue& value) {
    if (value.is_string()) {
        const std::string& text = value.as_string_ref();
        if (text.empty()) return 0;
        uint64_t parsed = 0;
        const char* begin = text.data();
        const char* end = begin + text.size();
        const auto result = std::from_chars(begin, end, parsed, 10);
        return result.ec == std::errc() && result.ptr == end ? parsed : 0;
    }
    // Legacy profiles stored identities as JSON numbers. Only the exactly
    // representable range is accepted; larger doubles may already be rounded.
    const double number = value.as_number(-1.0);
    constexpr double kLargestExactJsonInteger = 9007199254740992.0;
    if (!std::isfinite(number) || number < 0.0 || number > kLargestExactJsonInteger || std::floor(number) != number) { return 0; }
    return static_cast<uint64_t>(number);
}

std::string ExactUint64String(uint64_t value) {
    char buffer[21]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, 10);
    return result.ec == std::errc() ? std::string(buffer, result.ptr) : std::string();
}

// Keep only one record's JSON tree alive while building a store. Reuse the
// previous serialized size as a capacity hint; the final string still goes
// through the existing unchanged-write check, backups and atomic replacement.
class CompactJsonArray {
public:
    explicit CompactJsonArray(size_t previousBytes) {
        text.reserve(previousBytes);
        text.push_back('[');
    }
    void Append(const JsonValue& record) {
        record.append_to(BeginRecord());
    }
    std::string& BeginRecord() {
        if (text.size() > 1) text.push_back(',');
        return text;
    }
    std::string Finish() {
        text.push_back(']');
        return std::move(text);
    }
private:
    std::string text;
};

// Most library records have only the standard scalar fields. Serializing those
// directly avoids constructing and balancing a tree map for every record on
// each progress save. Keep the object keys in the same sorted order used by
// JsonValue's std::map serializer, and delegate scalar formatting/escaping to
// JsonValue so the emitted JSON retains the existing representation.
template <typename T>
void AppendJsonScalar(std::string& output, T&& value) {
    JsonValue(std::forward<T>(value)).append_to(output);
}

bool AppendSimpleMediaRecord(std::string& record, const MediaItem& item, const std::wstring& rootDir) {
    if ((item.extraFields.is_object() && !item.extraFields.as_object().empty()) ||
        (item.preferences.has_value() && item.preferences->HasAnyPreference()) ||
        !item.bookmarks.empty() || (item.overrides.has_value() && item.overrides->HasAnyOverride())) {
        return false;
    }

    // Append into the store buffer so each record needs no intermediate string.
    record.push_back('{');
    bool first = true;
    auto field = [&](const char* prefix, auto&& value) {
        if (!first) record.push_back(',');
        first = false;
        record += prefix;
        AppendJsonScalar(record, std::forward<decltype(value)>(value));
    };
    field("\"addedTimestamp\":", item.addedTimestamp);
    field("\"durationSec\":", item.durationSec);
    if (item.fileIndex != 0) field("\"fileIndex\":", ExactUint64String(item.fileIndex));
    field("\"filePath\":", Utf16ToUtf8(ToRelocatablePath(item.filePath, rootDir)));
    field("\"fileSizeBytes\":", item.fileSizeBytes);
    field("\"id\":", item.id);
    field("\"lastPosSec\":", item.lastPosSec);
    if (item.lastProbeAttemptTimestamp > 0) field("\"lastProbeAttemptTimestamp\":", item.lastProbeAttemptTimestamp);
    field("\"lastWatchedTimestamp\":", item.lastWatchedTimestamp);
    if (item.metadataProbeFailed) field("\"metadataProbeFailed\":", true);
    if (item.probeAttemptCount > 0) field("\"probeAttemptCount\":", static_cast<int>(item.probeAttemptCount));
    field("\"status\":", item.status);
    field("\"title\":", Utf16ToUtf8(item.title));
    field("\"videoHeight\":", static_cast<int>(item.videoHeight));
    field("\"videoWidth\":", static_cast<int>(item.videoWidth));
    if (item.volumeSerialNumber != 0) field("\"volumeSerialNumber\":", static_cast<uint64_t>(item.volumeSerialNumber));
    record.push_back('}');
    return true;
}


void AppendMediaRecord(std::string& output, const MediaItem& m, const std::wstring& rootDir) {
    if (AppendSimpleMediaRecord(output, m, rootDir)) return;
    JsonValue j = m.extraFields.is_object() ? m.extraFields : JsonValue::Object();
    j["id"] = m.id;
    j["filePath"] = Utf16ToUtf8(ToRelocatablePath(m.filePath, rootDir));
    j["title"] = Utf16ToUtf8(m.title);
    j["durationSec"] = m.durationSec;
    j["lastPosSec"] = m.lastPosSec;
    j["status"] = m.status;
    j["fileSizeBytes"] = m.fileSizeBytes;
    j["lastWatchedTimestamp"] = m.lastWatchedTimestamp;
    j["addedTimestamp"] = m.addedTimestamp;
    j["videoWidth"] = (int)m.videoWidth;
    j["videoHeight"] = (int)m.videoHeight;
    if (m.fileIndex != 0) j["fileIndex"] = ExactUint64String(m.fileIndex);
    if (m.volumeSerialNumber != 0) j["volumeSerialNumber"] = (uint64_t)m.volumeSerialNumber;
    if (m.metadataProbeFailed) {
        j["metadataProbeFailed"] = true;
    } else if (j.is_object()) { j.as_object().erase("metadataProbeFailed"); }
    if (m.probeAttemptCount > 0) {
        j["probeAttemptCount"] = (int)m.probeAttemptCount;
    } else if (j.is_object()) { j.as_object().erase("probeAttemptCount"); }
    if (m.lastProbeAttemptTimestamp > 0) {
        j["lastProbeAttemptTimestamp"] = m.lastProbeAttemptTimestamp;
    } else if (j.is_object()) { j.as_object().erase("lastProbeAttemptTimestamp"); }
    if (m.preferences.has_value() && m.preferences->HasAnyPreference()) {
        JsonValue prefObj = m.preferences->extraFields.is_object() ? m.preferences->extraFields : JsonValue::Object();
        const auto& prefs = *m.preferences;
        auto writePreference = [&](const char* key, const auto& value, bool valid = true) {
            if (value && valid) prefObj[key] = *value;
            else prefObj.as_object().erase(key);
        };
        writePreference("playbackSpeed", prefs.playbackSpeed,
            prefs.playbackSpeed && MediaPlaybackPreferences::IsValidPlaybackSpeed(*prefs.playbackSpeed));
        writePreference("aspectRatio", prefs.aspectRatio,
            prefs.aspectRatio && MediaPlaybackPreferences::IsValidAspectRatio(*prefs.aspectRatio));
        writePreference("audioTrackId", prefs.audioTrackId);
        writePreference("audioTrackTitle", prefs.audioTrackTitle);
        writePreference("audioTrackLang", prefs.audioTrackLang);
        writePreference("subtitleTrackId", prefs.subtitleTrackId);
        writePreference("subtitleTrackTitle", prefs.subtitleTrackTitle);
        writePreference("subtitleTrackLang", prefs.subtitleTrackLang);
        writePreference("audioDelayMs", prefs.audioDelayMs,
            prefs.audioDelayMs && MediaPlaybackPreferences::IsValidAudioDelayMs(*prefs.audioDelayMs));
        writePreference("subDelayMs", prefs.subDelayMs,
            prefs.subDelayMs && MediaPlaybackPreferences::IsValidSubDelayMs(*prefs.subDelayMs));
        writePreference("subtitlesVisible", prefs.subtitlesVisible);
        writePreference("videoRotate", prefs.videoRotate,
            prefs.videoRotate && MediaPlaybackPreferences::IsValidVideoRotate(*prefs.videoRotate));
        writePreference("brightness", prefs.brightness,
            prefs.brightness && MediaPlaybackPreferences::IsValidBrightness(*prefs.brightness));
        writePreference("contrast", prefs.contrast, prefs.contrast && MediaPlaybackPreferences::IsValidContrast(*prefs.contrast));
        writePreference("saturation", prefs.saturation,
            prefs.saturation && MediaPlaybackPreferences::IsValidSaturation(*prefs.saturation));
        writePreference("gamma", prefs.gamma, prefs.gamma && MediaPlaybackPreferences::IsValidGamma(*prefs.gamma));
        writePreference("hue", prefs.hue, prefs.hue && MediaPlaybackPreferences::IsValidHue(*prefs.hue));
        writePreference("audioBalance", prefs.audioBalance,
            prefs.audioBalance && MediaPlaybackPreferences::IsValidAudioBalance(*prefs.audioBalance));
        writePreference("audioChannelMap", prefs.audioChannelMap,
            prefs.audioChannelMap && MediaPlaybackPreferences::IsValidAudioChannelMap(*prefs.audioChannelMap));
        j["preferences"] = std::move(prefObj);
    } else if (j.is_object()) { j.as_object().erase("preferences"); }
    if (!m.bookmarks.empty()) {
        JsonValue bArray = JsonValue::Array();
        bArray.as_array().reserve(m.bookmarks.size());
        for (const auto& bm : m.bookmarks) {
            if (!bm.IsValid()) continue;
            JsonValue bObj = bm.extraFields.is_object() ? bm.extraFields : JsonValue::Object();
            bObj["id"] = bm.id;
            bObj["timeSec"] = bm.timeSec;
            bObj["label"] = Utf16ToUtf8(bm.label);
            if (bm.createdTimestamp > 0) { bObj["createdTimestamp"] = bm.createdTimestamp; }
            bArray.push_back(std::move(bObj));
        }
        if (!bArray.as_array().empty()) {
            j["bookmarks"] = std::move(bArray);
        } else if (j.is_object()) { j.as_object().erase("bookmarks"); }
    } else if (j.is_object()) { j.as_object().erase("bookmarks"); }
    if (m.overrides.has_value() && m.overrides->HasAnyOverride()) {
        JsonValue ovObj = m.overrides->extraFields.is_object() ? m.overrides->extraFields : JsonValue::Object();
        if (m.overrides->title.has_value() && MediaMetadataOverrides::IsValidTitle(*m.overrides->title)) {
            ovObj["title"] = Utf16ToUtf8(*m.overrides->title);
        } else if (ovObj.is_object()) { ovObj.as_object().erase("title"); }
        if (m.overrides->year.has_value() && MediaMetadataOverrides::IsValidYear(*m.overrides->year)) {
            ovObj["year"] = *m.overrides->year;
        } else if (ovObj.is_object()) { ovObj.as_object().erase("year"); }
        if (m.overrides->series.has_value() && MediaMetadataOverrides::IsValidSeries(*m.overrides->series)) {
            ovObj["series"] = Utf16ToUtf8(*m.overrides->series);
        } else if (ovObj.is_object()) { ovObj.as_object().erase("series"); }
        j["overrides"] = std::move(ovObj);
    } else if (j.is_object()) { j.as_object().erase("overrides"); j.as_object().erase("metadataOverrides"); }
    j.append_to(output);
}

// Resource optimization: retain only unknown JSON fields in extraFields.
// Typed members already hold every known field; keeping the full DOM per
// item duplicates typed state and costs tens of MiB at 25k items. Save() overlays typed
// fields onto the sparse object, so round-trips preserve unknown keys byte-semantically.
inline void StripKnownKeys(JsonValue& obj, const char* const* keys, size_t count) {
    if (!obj.is_object()) return;
    auto& map = obj.as_object();
    for (size_t i = 0; i < count; ++i) {
        const auto found = map.find(keys[i]);
        if (found != map.end()) map.erase(found);
    }
}

struct DiscoveredFile { std::wstring path; std::wstring normalizedKey; };

// mfreadwrite/mfplat are delay-loaded: POD-only delay gates (no C++ objects, so __try is legal). Callers hold C++ objects and must never
// use __try themselves (C2712).
HRESULT DelaySafeMFCreateAttributes(IMFAttributes** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!SystemDllPresent(L"MFPlat.DLL")) return E_FAIL;
    HRESULT hr = E_FAIL;
    __try {
        hr = MFCreateAttributes(out, 1);
    } __except (DelayLoadExceptionFilter(GetExceptionCode())) { hr = E_FAIL; *out = nullptr; }
    return hr;
}

HRESULT DelaySafeMFCreateSourceReader(const wchar_t* url, IMFAttributes* attrs, IMFSourceReader** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!url || !SystemDllPresent(L"MFReadWrite.dll")) return E_FAIL;
    HRESULT hr = E_FAIL;
    __try {
        hr = MFCreateSourceReaderFromURL(url, attrs, out);
    } __except (DelayLoadExceptionFilter(GetExceptionCode())) { hr = E_FAIL; *out = nullptr; }
    return hr;
}

} // namespace

const std::set<std::wstring>& MediaLibrary::GetSupportedExtensions() {
    static const std::set<std::wstring> extensions(kSupportedExtensionValues.begin(), kSupportedExtensionValues.end());
    return extensions;
}

static bool MigrateArraySchema(JsonValue& root, int fromVersion, int targetVersion, const char* key, int version) {
    if (fromVersion >= targetVersion) return true;
    for (int v = fromVersion; v < targetVersion; ++v) {
        if (v >= 0 && v < version) {
            if (root.is_array()) {
                JsonValue obj = JsonValue::Object();
                obj["schemaVersion"] = version;
                obj[key] = std::move(root);
                root = std::move(obj);
            } else if (root.is_object()) {
                root["schemaVersion"] = version;
                if (!root.has_key(key) || !root.get(key).is_array()) { root[key] = JsonValue::Array(); }
            }
        }
    }
    return true;
}
bool MediaLibrary::MigrateLibrarySchema(JsonValue& root, int fromVersion, int targetVersion) {
    return MigrateArraySchema(root, fromVersion, targetVersion, "items", 2);
}
bool MediaLibrary::MigratePlaylistsSchema(JsonValue& root, int fromVersion, int targetVersion) {
    return MigrateArraySchema(root, fromVersion, targetVersion, "items", 2);
}
bool MediaLibrary::MigrateHistorySchema(JsonValue& root, int fromVersion, int targetVersion) {
    return MigrateArraySchema(root, fromVersion, targetVersion, "items", 2);
}
bool MediaLibrary::MigrateWatchedFoldersSchema(JsonValue& root, int fromVersion, int targetVersion) {
    return MigrateArraySchema(root, fromVersion, targetVersion, "folders", 1);
}
MediaLibrary::MediaLibrary() : portableRoot(GetExeDir()) {}

std::string MediaLibrary::GenerateId(const std::wstring& path) {
    std::string s = Utf16ToUtf8(NormalizePathKey(path));
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char b[48];
    sprintf_s(b, "media_%016llx", (unsigned long long)h);
    std::string baseId = b;
    if (idToIndex.empty() && !mediaItems.empty()) { RebuildPathIndex(); }
    if (idToIndex.find(baseId) == idToIndex.end()) { return baseId; }
    for (unsigned i = 1;; ++i) {
        std::string candidate = baseId + "_" + std::to_string(i);
        if (idToIndex.find(candidate) == idToIndex.end()) { return candidate; }
    }
}

bool MediaLibrary::ProbeMediaMetadata(const std::wstring& path, double& d, uint32_t& w, uint32_t& h) {
    d = 0;
    w = 0;
    h = 0;
    bool isolated = false;
    const bool isolatedResult = TryIsolatedMetadataProbe(path, d, w, h, isolated);
    if (isolated) return isolatedResult;
    // Lazy MF: no startup cost until the first real probe. Nested acquires from import/backfill jobs only bump the refcount. Fails
    // gracefully when the (delay-loaded) MF DLLs are unavailable.
    MediaFoundation::ScopedAcquire mfGuard;
    if (!mfGuard.held()) return false;
    IMFAttributes* rawAttrs = nullptr;
    HRESULT hr = DelaySafeMFCreateAttributes(&rawAttrs);
    if (FAILED(hr) || !rawAttrs) return false;
    ComPtr<IMFAttributes> a;
    a.Attach(rawAttrs);
    a->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
    IMFSourceReader* rawReader = nullptr;
    hr = DelaySafeMFCreateSourceReader(path.c_str(), a.Get(), &rawReader);
    if (FAILED(hr) || !rawReader) return false;
    ComPtr<IMFSourceReader> r;
    r.Attach(rawReader);
    PROPVARIANT v;
    PropVariantInit(&v);
    hr = r->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v);
    if (SUCCEEDED(hr) && v.vt == VT_UI8) { d = (double)v.uhVal.QuadPart / 10000000.0; }
    PropVariantClear(&v);
    ComPtr<IMFMediaType> t;
    hr = r->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &t);
    if (SUCCEEDED(hr) && t) { MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h); }
    return true;
}

std::wstring MediaLibrary::NormalizePathKey(const std::wstring& path) { return Identity(path); }

void MediaLibrary::RebuildPathIndex() {
    ++revisions.catalog; ++revisions.history;
    ++searchContentVersion; // Membership/path changes also affect search text.
    pathToIndex.clear();
    idToIndex.clear();
    pathToIndex.reserve(mediaItems.size());
    idToIndex.reserve(mediaItems.size());
    for (size_t i = 0; i < mediaItems.size(); ++i) {
        std::wstring key = NormalizePathKey(mediaItems[i].filePath);
        if (!key.empty()) { pathToIndex[key] = i; }
        if (!mediaItems[i].id.empty()) { idToIndex[mediaItems[i].id] = i; }
    }
}

bool MediaLibrary::PromoteMostRecentlyWatched() {
    if (mediaItems.empty()) return false;
    auto best = std::max_element(mediaItems.begin(), mediaItems.end(),
                                 [](const MediaItem &a, const MediaItem &b) { return a.lastWatchedTimestamp < b.lastWatchedTimestamp; });
    if (best == mediaItems.end() || best == mediaItems.begin() || best->lastWatchedTimestamp <= 0) { return false; }
    const size_t bestIndex = static_cast<size_t>(std::distance(mediaItems.begin(), best));
    std::iter_swap(mediaItems.begin(), best);
    // Load() normally leaves both maps complete. If a caller constructed the
    // public mediaItems vector manually, rebuild once rather than publishing partially repaired lookup state.
    if ((pathToIndex.empty() && !mediaItems.empty()) || (idToIndex.empty() && !mediaItems.empty())) { RebuildPathIndex(); } else {
        const auto repair = [this](size_t index) {
            const MediaItem& item = mediaItems[index];
            const std::wstring pathKey = NormalizePathKey(item.filePath);
            if (!pathKey.empty()) pathToIndex[pathKey] = index;
            if (!item.id.empty()) idToIndex[item.id] = index;
        };
        repair(0);
        repair(bestIndex);
    }
    ++libraryVersion; ++revisions.catalog;
    InvalidatePlaylistStatsCache();
    return true;
}

MediaRelocationResult MediaLibrary::RelocateMissingMediaToRoot(const std::wstring& searchRoot) {
    MediaRelocationResult result;
    const std::wstring resolvedRoot = ResolveAppPath(searchRoot);
    const DWORD rootAttributes = GetFileAttributesW(resolvedRoot.c_str());
    if (resolvedRoot.empty() || rootAttributes == INVALID_FILE_ATTRIBUTES || !(rootAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return result;
    auto filenameKey = [](const std::wstring& path) {
        std::wstring key = GetFileNameFromPath(path);
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        return key;
    };
    std::unordered_map<std::wstring, std::vector<std::wstring>> candidatesByName;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(resolvedRoot, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const auto& entry = *iterator;
        if (entry.is_regular_file(error)) {
            const std::wstring candidate = entry.path().wstring();
            if (IsSupportedMediaExtension(candidate)) { candidatesByName[filenameKey(candidate)].push_back(candidate); }
        }
        iterator.increment(error);
        if (error) error.clear();
    }
    std::unordered_set<std::wstring> claimedTargets;
    std::unordered_map<std::wstring, std::wstring> replacements;
    for (auto& item : mediaItems) {
        if (FileExists(item.filePath)) continue;
        result.missingCount++;
        const auto candidates = candidatesByName.find(filenameKey(item.filePath));
        if (candidates == candidatesByName.end() || candidates->second.empty()) { result.notFoundCount++; continue; }
        if (candidates->second.size() != 1) { result.ambiguousCount++; continue; }
        const std::wstring replacement = candidates->second.front();
        const std::wstring replacementKey = NormalizePathKey(replacement);
        const MediaItem* existing = FindByPath(replacement);
        if (replacementKey.empty() || claimedTargets.find(replacementKey) != claimedTargets.end() || (existing && existing != &item)) {
            result.ambiguousCount++;
            continue;
        }
        const std::wstring oldPath = item.filePath;
        replacements.emplace(NormalizePathKey(oldPath), replacement);
        claimedTargets.insert(replacementKey);
        item.filePath = replacement;
        item.fileSizeBytes = GetFileSizeInBytes(replacement);
        const WindowsFileIdentity identity = GetWindowsFileIdentity(replacement);
        if (identity.valid) { item.fileIndex = identity.fileIndex; item.volumeSerialNumber = identity.volumeSerialNumber; }
        result.relocatedPaths.emplace_back(oldPath, replacement);
        result.relocatedCount++;
    }
    if (result.relocatedCount == 0) return result;
    auto relocateReference = [&](std::wstring& path) {
        const auto replacement = replacements.find(NormalizePathKey(path));
        if (replacement != replacements.end()) path = replacement->second;
    };
    for (auto& playlist : playlists) { for (auto& path : playlist.itemPaths) relocateReference(path); }
    for (auto& record : history) relocateReference(record.filePath);
    RebuildPathIndex();
    NotifyPlaylistChanged();
    return result;
}

FolderRediscoveryResult MediaLibrary::RediscoverFolderChanges(bool saveChanges) {
    return ApplyFolderRediscovery(PrepareFolderRediscovery(FolderRediscoverySnapshot::Capture(*this)), saveChanges);
}

FolderRediscoveryResult MediaLibrary::ApplyFolderRediscovery(const PreparedFolderRediscovery& prepared, bool saveChanges) {
    FolderRediscoveryResult result;
    if (prepared.canceled) return result;
    result.status = prepared.status;
    if (prepared.moves.empty()) return result;
    std::unordered_map<std::string, size_t> ids;
    std::unordered_map<std::wstring, size_t> paths;
    ids.reserve(mediaItems.size()); paths.reserve(mediaItems.size());
    for (size_t i = 0; i < mediaItems.size(); ++i) {
        if (!mediaItems[i].id.empty()) ids[mediaItems[i].id] = i;
        paths[NormalizePathKey(mediaItems[i].filePath)] = i;
    }
    std::vector<bool> removed(mediaItems.size(), false), acceptedFolders(prepared.folders.size(), false);
    struct Reference { std::wstring path, title; };
    std::unordered_map<std::wstring, Reference> references;
    for (const auto& move : prepared.moves) {
        if (move.folder >= prepared.folders.size()) continue;
        const auto oldKey = NormalizePathKey(move.source.path), newKey = NormalizePathKey(move.path);
        if (oldKey.empty() || newKey.empty() || oldKey == newKey) continue;
        size_t sourceIndex = SIZE_MAX;
        if (move.source.id.empty()) {
            const auto found = paths.find(oldKey);
            if (found != paths.end()) sourceIndex = found->second;
        } else {
            const auto found = ids.find(move.source.id);
            if (found != ids.end()) sourceIndex = found->second;
        }
        if (sourceIndex == SIZE_MAX || removed[sourceIndex]) continue;
        auto& source = mediaItems[sourceIndex];
        if (!move.source.Matches(source)) { result.needsRescan = true; continue; }
        auto target = paths.find(newKey);
        const bool hasTarget = target != paths.end() && !removed[target->second];
        // A target imported, replaced or edited while discovery ran needs a
        // fresh baseline before any duplicate record can be removed.
        if (hasTarget != move.destination.has_value() ||
            (hasTarget && !move.destination->Matches(mediaItems[target->second]))) {
            result.needsRescan = true; continue;
        }
        if (hasTarget) {
            auto& live = mediaItems[target->second];
            if (live.lastPosSec <= 0 && source.lastPosSec > 0) live.lastPosSec = source.lastPosSec;
            if (live.lastWatchedTimestamp == 0 && source.lastWatchedTimestamp > 0) live.lastWatchedTimestamp = source.lastWatchedTimestamp;
            if (live.status == "new" && source.status != "new") live.status = source.status;
            if (live.durationSec <= 0 && source.durationSec > 0) live.durationSec = source.durationSec;
            if (live.bookmarks.empty() && !source.bookmarks.empty()) live.bookmarks = source.bookmarks;
            if (!live.preferences && source.preferences) live.preferences = source.preferences;
            if (!live.overrides && source.overrides) live.overrides = source.overrides;
            references[oldKey] = {live.filePath, live.title};
            removed[sourceIndex] = true;
            ++result.mergedItemsCount;
            ++result.removedGhostCount;
        } else {
            const bool changedSize = source.fileSizeBytes != move.size;
            source.filePath = move.path; source.title = GetFileNameFromPath(move.path); source.fileSizeBytes = move.size;
            if (move.identity.valid) { source.fileIndex = move.identity.fileIndex; source.volumeSerialNumber = move.identity.volumeSerialNumber; }
            if (changedSize) { source.metadataProbeFailed = true; source.probeAttemptCount = 0; source.lastProbeAttemptTimestamp = 0; }
            paths[newKey] = sourceIndex;
            references[oldKey] = {source.filePath, source.title};
            ++result.relocatedItemsCount;
        }
        paths.erase(oldKey);
        acceptedFolders[move.folder] = true;
    }
    if (references.empty()) return result;
    const auto relocate = [&](std::wstring& path, std::wstring* title = nullptr) {
        bool changed = false;
        for (size_t i = 0; i < references.size(); ++i) {
            const auto found = references.find(NormalizePathKey(path));
            if (found == references.end() || path == found->second.path) break;
            path = found->second.path;
            if (title) *title = found->second.title;
            changed = true;
        }
        return changed;
    };
    bool playlistsChanged = false, historyChanged = false;
    for (auto& playlist : playlists) for (auto& path : playlist.itemPaths) playlistsChanged |= relocate(path);
    for (auto& item : history) historyChanged |= relocate(item.filePath, &item.title);
    std::unordered_map<std::wstring, std::wstring> folderMoves;
    for (size_t i = 0; i < acceptedFolders.size(); ++i) if (acceptedFolders[i]) {
        result.renamedFolders.push_back(prepared.folders[i]);
        folderMoves[NormalizePathKey(prepared.folders[i].first)] = prepared.folders[i].second;
    }
    result.renamedFoldersCount = result.renamedFolders.size();
    for (auto& root : watchedFolders) {
        const auto found = folderMoves.find(NormalizePathKey(root));
        if (found != folderMoves.end()) root = found->second;
    }
    if (result.removedGhostCount) {
        size_t write = 0;
        for (size_t read = 0; read < mediaItems.size(); ++read) {
            if (removed[read]) continue;
            if (read != write) mediaItems[write] = std::move(mediaItems[read]);
            ++write;
        }
        mediaItems.resize(write);
    }
    if (playlistsChanged) ++revisions.playlists;
    if (historyChanged) ++revisions.history;
    RebuildPathIndex();
    ++libraryVersion;
    InvalidatePlaylistStatsCache();
    if (saveChanges) Save();
    return result;
}

size_t MediaLibrary::RemoveFolder(const std::wstring& folderPath, bool saveChanges) {
    if (folderPath.empty()) return 0;
    std::wstring norm = NormalizeDirectoryPath(folderPath);
    if (!norm.empty() && norm.back() != L'\\' && norm.back() != L'/') { norm += L'\\'; }
    std::vector<std::string> idsToRemove;
    for (const auto& item : mediaItems) {
        std::wstring itemDir = NormalizeDirectoryPath(ExtractDirectory(item.filePath));
        if (!itemDir.empty() && itemDir.back() != L'\\' && itemDir.back() != L'/') { itemDir += L'\\'; }
        if (itemDir.size() >= norm.size() && _wcsnicmp(itemDir.c_str(), norm.c_str(), norm.size()) == 0) { idsToRemove.push_back(item.id); }
    }
    size_t removed = 0;
    if (!idsToRemove.empty()) { removed = RemoveMedia(idsToRemove); }
    auto it = std::remove_if(watchedFolders.begin(), watchedFolders.end(), [&](const std::wstring& wf) {
        std::wstring wfNorm = NormalizeDirectoryPath(wf);
        if (!wfNorm.empty() && wfNorm.back() != L'\\' && wfNorm.back() != L'/') { wfNorm += L'\\'; }
        return (wfNorm == norm || (wfNorm.size() >= norm.size() && _wcsnicmp(wfNorm.c_str(), norm.c_str(), norm.size()) == 0));
    });
    if (it != watchedFolders.end()) { watchedFolders.erase(it, watchedFolders.end()); }
    if (removed > 0 || it != watchedFolders.end()) {
        RebuildPathIndex();
        libraryVersion++;
        InvalidatePlaylistStatsCache();
        if (saveChanges) { Save(); }
    }
    return removed;
}

size_t MediaLibrary::RemoveFolders(const std::vector<std::wstring>& folderPaths, bool saveChanges) {
    if (folderPaths.empty()) return 0;
    std::vector<std::wstring> normFolders;
    normFolders.reserve(folderPaths.size());
    for (const auto& fp : folderPaths) {
        if (fp.empty()) continue;
        std::wstring n = NormalizeDirectoryPath(fp);
        if (!n.empty() && n.back() != L'\\' && n.back() != L'/') { n += L'\\'; }
        normFolders.push_back(n);
    }
    std::vector<std::string> idsToRemove;
    for (const auto& item : mediaItems) {
        std::wstring itemDir = NormalizeDirectoryPath(ExtractDirectory(item.filePath));
        if (!itemDir.empty() && itemDir.back() != L'\\' && itemDir.back() != L'/') { itemDir += L'\\'; }
        for (const auto& norm : normFolders) {
            if (itemDir.size() >= norm.size() && _wcsnicmp(itemDir.c_str(), norm.c_str(), norm.size()) == 0) {
                idsToRemove.push_back(item.id);
                break;
            }
        }
    }
    size_t removed = 0;
    if (!idsToRemove.empty()) { removed = RemoveMedia(idsToRemove); }
    bool watchedChanged = false;
    for (const auto& norm : normFolders) {
        auto it = std::remove_if(watchedFolders.begin(), watchedFolders.end(), [&](const std::wstring& wf) {
            std::wstring wfNorm = NormalizeDirectoryPath(wf);
            if (!wfNorm.empty() && wfNorm.back() != L'\\' && wfNorm.back() != L'/') { wfNorm += L'\\'; }
            return (wfNorm == norm || (wfNorm.size() >= norm.size() && _wcsnicmp(wfNorm.c_str(), norm.c_str(), norm.size()) == 0));
        });
        if (it != watchedFolders.end()) { watchedFolders.erase(it, watchedFolders.end()); watchedChanged = true; }
    }
    if (removed > 0 || watchedChanged) {
        RebuildPathIndex();
        libraryVersion++;
        InvalidatePlaylistStatsCache();
        if (saveChanges) { Save(); }
    }
    return removed;
}

void MediaLibrary::InvalidatePlaylistStatsCache(bool clearSmartPaths) const {
    playlistStatsCache.clear();
    if (clearSmartPaths) { smartPathsCache.clear(); smartAvailability.clear(); }
}

void MediaLibrary::NotifyPlaylistChanged() {
    ++libraryVersion; ++searchContentVersion;
    ++revisions.catalog; ++revisions.playback; ++revisions.playlists; ++revisions.history;
    InvalidatePlaylistStatsCache();
}

void MediaLibrary::NotifyPlaylistEdited() {
    ++libraryVersion; ++revisions.playlists;
    InvalidatePlaylistStatsCache();
}

PlaylistStats MediaLibrary::GetPlaylistStats(const std::string& playlistId) const {
    auto it = playlistStatsCache.find(playlistId);
    if (it != playlistStatsCache.end() && it->second.revisions.SamePlaylistData(revisions) &&
        GetTickCount64() < it->second.expiresAtTick) {
        const auto& entry = it->second;
        const int64_t now = entry.timeDependent ? GetCurrentTimestampSec() : 0;
        if (!entry.timeDependent || (now >= entry.evaluatedAtSec && now < entry.expiresAtSec)) return entry.stats;
    }
    PlaylistStats stats;
    const Playlist* pl = FindPlaylist(playlistId);
    if (!pl) return stats;
    const auto& paths = GetPlaylistPathsRef(*pl);
    stats.totalCount = (int)paths.size();
    size_t firstUnfinished = paths.size(), firstInProgress = paths.size();
    for (size_t i = 0; i < paths.size(); ++i) {
        const MediaItem* mi = FindByPath(paths[i]);
        if ((!mi || mi->status == "new") && firstUnfinished == paths.size()) firstUnfinished = i;
        if (mi && mi->status == "in_progress" && firstInProgress == paths.size()) firstInProgress = i;
        if (mi) {
            stats.totalSizeBytes += mi->fileSizeBytes;
            stats.totalDurationSec += mi->durationSec;
            if ((mi->durationSec > 0.0 && (mi->lastPosSec / mi->durationSec) >= 0.90) || (mi->status == "done")) { stats.watchedCount++; }
        }
    }
    // Match GetPlaylistResumeIndex: prefer any in-progress item, then the
    // first new/missing record, and otherwise start at the first item.
    if (firstInProgress < paths.size()) stats.resumeIndex = firstInProgress;
    else if (firstUnfinished < paths.size()) stats.resumeIndex = firstUnfinished;
    PlaylistStatsCacheEntry entry{ revisions, stats };
    if (pl->IsSmart()) {
        // Membership and aggregates expire together, including relative dates.
        const auto& pathEntry = smartPathsCache.at(playlistId);
        entry.expiresAtTick = pathEntry.expiresAtTick;
        entry.evaluatedAtSec = pathEntry.evaluatedAtSec;
        entry.expiresAtSec = pathEntry.expiresAtSec;
        entry.timeDependent = pathEntry.timeDependent;
    }
    playlistStatsCache[playlistId] = entry;
    return stats;
}

MediaItem* MediaLibrary::FindByPath(const std::wstring& p) {
    if (p.empty() || mediaItems.empty()) return nullptr;
    if (pathToIndex.empty() && !mediaItems.empty()) RebuildPathIndex();
    std::wstring key = NormalizePathKey(p);
    auto it = pathToIndex.find(key);
    if (it != pathToIndex.end() && it->second < mediaItems.size()) { return &mediaItems[it->second]; }
    return nullptr;
}

const MediaItem* MediaLibrary::FindByPath(const std::wstring& p) const {
    if (p.empty() || mediaItems.empty()) return nullptr;
    if (pathToIndex.empty() && !mediaItems.empty()) const_cast<MediaLibrary*>(this)->RebuildPathIndex();
    std::wstring key = NormalizePathKey(p);
    auto it = pathToIndex.find(key);
    if (it != pathToIndex.end() && it->second < mediaItems.size()) { return &mediaItems[it->second]; }
    return nullptr;
}

MediaItem* MediaLibrary::FindById(const std::string& id) {
    if (id.empty() || mediaItems.empty()) return nullptr;
    if (idToIndex.empty() && !mediaItems.empty()) RebuildPathIndex();
    auto it = idToIndex.find(id);
    if (it != idToIndex.end() && it->second < mediaItems.size()) { return &mediaItems[it->second]; }
    return nullptr;
}

const MediaItem* MediaLibrary::FindById(const std::string& id) const {
    if (id.empty() || mediaItems.empty()) return nullptr;
    if (idToIndex.empty() && !mediaItems.empty()) const_cast<MediaLibrary*>(this)->RebuildPathIndex();
    auto it = idToIndex.find(id);
    if (it != idToIndex.end() && it->second < mediaItems.size()) { return &mediaItems[it->second]; }
    return nullptr;
}

const MediaPlaybackPreferences* MediaLibrary::GetMediaPreferences(const std::wstring& path) const {
    const MediaItem* item = FindByPath(path);
    if (item && item->preferences.has_value()) { return &(*item->preferences); }
    return nullptr;
}

bool MediaLibrary::SetMediaPreferences(const std::wstring& path, const MediaPlaybackPreferences& prefs) {
    MediaItem* item = FindByPath(path);
    if (!item) item = AddOrUpdateMedia(path);
    if (item) { item->preferences = prefs; libraryVersion++; return true; }
    return false;
}

const std::vector<MediaBookmark>* MediaLibrary::GetBookmarks(const std::wstring& path) const {
    const MediaItem* item = FindByPath(path);
    if (!item) return nullptr;
    return &item->bookmarks;
}

bool MediaLibrary::AddBookmark(const std::wstring& path, const MediaBookmark& bookmark) {
    if (!bookmark.IsValid()) return false;
    MediaItem* item = FindByPath(path);
    if (!item) { item = AddOrUpdateMedia(path); if (!item) return false; }
    MediaBookmark validated = bookmark;
    if (validated.label.length() > MediaBookmark::kMaxLabelLength)
        validated.label = validated.label.substr(0, MediaBookmark::kMaxLabelLength);
    if (validated.id.empty())
        validated.id = GenerateId(item->filePath + L"_" + std::to_wstring(validated.timeSec) + L"_" + std::to_wstring(validated.createdTimestamp));
    auto it = std::find_if(item->bookmarks.begin(), item->bookmarks.end(), [&](const MediaBookmark& b) { return b.id == validated.id; });
    if (it != item->bookmarks.end()) { *it = validated; } else { item->bookmarks.push_back(std::move(validated)); }
    std::stable_sort(item->bookmarks.begin(), item->bookmarks.end(), MediaBookmark::Compare);
    libraryVersion++;
    return true;
}

bool MediaLibrary::RemoveBookmark(const std::wstring& path, const std::string& bookmarkId) {
    if (bookmarkId.empty()) return false;
    MediaItem* item = FindByPath(path);
    if (!item) return false;
    auto it = std::find_if(item->bookmarks.begin(), item->bookmarks.end(), [&](const MediaBookmark& b) { return b.id == bookmarkId; });
    if (it == item->bookmarks.end()) return false;
    item->bookmarks.erase(it);
    libraryVersion++;
    return true;
}

bool MediaLibrary::ClearBookmarks(const std::wstring& path) {
    MediaItem* item = FindByPath(path);
    if (!item || item->bookmarks.empty()) return false;
    item->bookmarks.clear();
    libraryVersion++;
    return true;
}
const MediaMetadataOverrides* MediaLibrary::GetMetadataOverrides(const std::wstring& path) const {
    const MediaItem* item = FindByPath(path);
    if (!item || !item->overrides.has_value() || !item->overrides->HasAnyOverride()) { return nullptr; }
    return &(*item->overrides);
}

bool MediaLibrary::SetMetadataOverrides(const std::wstring& path, const MediaMetadataOverrides& overrides) {
    MediaItem* item = FindByPath(path);
    if (!item) item = AddOrUpdateMedia(path);
    if (item) {
        const auto oldTitle = item->GetDisplayTitle();
        if (overrides.HasAnyOverride()) { item->overrides = overrides; } else { item->overrides.reset(); }
        if (oldTitle != item->GetDisplayTitle()) ++searchContentVersion;
        libraryVersion++; ++revisions.catalog;
        InvalidatePlaylistStatsCache();
        return true;
    }
    return false;
}

bool MediaLibrary::ClearMetadataOverrides(const std::wstring& path) {
    MediaItem* item = FindByPath(path);
    if (item && item->overrides.has_value()) {
        const auto oldTitle = item->GetDisplayTitle();
        item->overrides.reset();
        if (oldTitle != item->GetDisplayTitle()) ++searchContentVersion;
        libraryVersion++; ++revisions.catalog; InvalidatePlaylistStatsCache(); return true;
    }
    return false;
}

bool MediaLibrary::RefreshMediaMetadata(const std::wstring& path, bool saveChanges, const MediaProbeFunction& probeFn) {
    MediaItem* item = FindByPath(path);
    if (!item) return false;
    const bool fileExists = FileExists(item->filePath);
    const auto probe = probeFn ? probeFn : ProbeMediaMetadata;
    double duration = 0.0;
    uint32_t width = 0, height = 0;
    if (fileExists && probe(item->filePath, duration, width, height)) {
        item->durationSec = std::max(0.0, duration);
        item->videoWidth = width;
        item->videoHeight = height;
        item->fileSizeBytes = GetFileSizeInBytes(item->filePath);
        const WindowsFileIdentity fid = GetWindowsFileIdentity(item->filePath);
        if (fid.valid) { item->fileIndex = fid.fileIndex; item->volumeSerialNumber = fid.volumeSerialNumber; }
        if (item->durationSec > 0.0) { item->lastPosSec = std::min(item->lastPosSec, item->durationSec); }
        item->metadataProbeFailed = false;
        item->probeAttemptCount = 0;
        item->lastProbeAttemptTimestamp = 0;
        libraryVersion++; ++revisions.catalog;
        InvalidatePlaylistStatsCache();
        if (saveChanges) Save();
        return true;
    } else {
        item->metadataProbeFailed = true;
        item->probeAttemptCount++;
        item->lastProbeAttemptTimestamp = GetCurrentTimestampSec();
        libraryVersion++; ++revisions.catalog;
        InvalidatePlaylistStatsCache();
        if (saveChanges) Save();
        return false;
    }
}
bool MediaLibrary::IsSupportedMediaExtension(const std::wstring& path) {
    // Windows uses colons for alternate data streams, and device/UNC roots
    // have parser rules beyond ordinary filename suffixes. Preserve the
    // standard-library behavior for those uncommon forms.
    const bool hasUncOrDeviceRoot = path.size() >= 2 &&
        (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/');
    const bool hasNonDriveColon = path.find(L':', 2) != std::wstring::npos;
    if (hasUncOrDeviceRoot || hasNonDriveColon) {
        const std::wstring ext = LowerExtension(path);
        return GetSupportedExtensions().find(ext) != GetSupportedExtensions().end();
    }
    return IsSupportedExtensionValue(PathExtension(path));
}

MediaItem* MediaLibrary::AddOrUpdateMediaCore(const std::wstring& path, bool& changed, bool& metadataProbeFailed, bool probeMetadata) {
    changed = false;
    metadataProbeFailed = false;
    if (ClassifyPlaybackTarget(path) != PlaybackTargetKind::LocalFile) return nullptr;
    std::wstring resolvedPath = ResolveAppPath(path);
    const bool exists = FileExists(resolvedPath);
    const uint64_t size = exists ? GetFileSizeInBytes(resolvedPath) : 0;
    MediaItem* m = FindByPath(resolvedPath);
    auto probe = [&](double& duration, uint32_t& w, uint32_t& h) {
        if (!ProbeMediaMetadata(resolvedPath, duration, w, h)) { metadataProbeFailed = true; return false; }
        duration = std::max(0.0, duration);
        return true;
    };
    if (m) {
        if (exists && m->fileSizeBytes != size) {
            m->fileSizeBytes = size;
            changed = true;
            if (!probeMetadata) {
                // Preserve usable metadata until the owned backfill replaces it,
                // while making a changed file immediately eligible for probing.
                m->metadataProbeFailed = true;
                m->probeAttemptCount = 0;
                m->lastProbeAttemptTimestamp = 0;
            } else {
            double d;
            uint32_t w, h;
            if (probe(d, w, h)) {
                m->durationSec = d;
                m->videoWidth = w;
                m->videoHeight = h;
                m->metadataProbeFailed = false;
                m->probeAttemptCount = 0;
                m->lastProbeAttemptTimestamp = 0;
                changed = true;
            } else {
                m->metadataProbeFailed = true;
                m->probeAttemptCount++;
                m->lastProbeAttemptTimestamp = GetCurrentTimestampSec();
                changed = true;
            }
            }
            WindowsFileIdentity fid = GetWindowsFileIdentity(resolvedPath);
            if (fid.valid) { m->fileIndex = fid.fileIndex; m->volumeSerialNumber = fid.volumeSerialNumber; }
        } else if (exists && m->durationSec <= 0 && probeMetadata) {
            double d;
            uint32_t w, h;
            if (probe(d, w, h)) {
                m->durationSec = d;
                m->videoWidth = w;
                m->videoHeight = h;
                m->metadataProbeFailed = false;
                m->probeAttemptCount = 0;
                m->lastProbeAttemptTimestamp = 0;
                changed = true;
            } else {
                m->metadataProbeFailed = true;
                m->probeAttemptCount++;
                m->lastProbeAttemptTimestamp = GetCurrentTimestampSec();
                changed = true;
            }
            WindowsFileIdentity fid = GetWindowsFileIdentity(resolvedPath);
            if (fid.valid) { m->fileIndex = fid.fileIndex; m->volumeSerialNumber = fid.volumeSerialNumber; }
        }
        if (changed) { libraryVersion++; ++revisions.catalog; InvalidatePlaylistStatsCache(); }
        return m;
    }
    MediaItem x;
    x.id = GenerateId(resolvedPath);
    x.filePath = resolvedPath;
    x.title = GetFileNameFromPath(resolvedPath);
    x.fileSizeBytes = size;
    x.addedTimestamp = GetCurrentTimestampSec();
    if (exists && probeMetadata) {
        double d;
        uint32_t w, h;
        if (probe(d, w, h)) {
            x.durationSec = d;
            x.videoWidth = w;
            x.videoHeight = h;
            x.metadataProbeFailed = false;
            x.probeAttemptCount = 0;
            x.lastProbeAttemptTimestamp = 0;
        } else { x.metadataProbeFailed = true; x.probeAttemptCount = 1; x.lastProbeAttemptTimestamp = GetCurrentTimestampSec(); }
        WindowsFileIdentity fid = GetWindowsFileIdentity(resolvedPath);
        if (fid.valid) { x.fileIndex = fid.fileIndex; x.volumeSerialNumber = fid.volumeSerialNumber; }
    }
    mediaItems.insert(mediaItems.begin(), std::move(x));
    ++searchContentVersion;
    changed = true;
    // Inserting at the front shifts every existing index by one. Adjust the
    // stored indices in place instead of re-normalizing every path key; a full
    // rebuild is only needed when the lookup maps were never populated.
    if (((pathToIndex.empty() && mediaItems.size() > 1) || (idToIndex.empty() && mediaItems.size() > 1))) {
        RebuildPathIndex();
    } else {
        for (auto& entry : pathToIndex) { ++entry.second; }
        for (auto& entry : idToIndex) { ++entry.second; }
        const std::wstring frontKey = NormalizePathKey(mediaItems.front().filePath);
        if (!frontKey.empty()) { pathToIndex[frontKey] = 0; }
        if (!mediaItems.front().id.empty()) { idToIndex[mediaItems.front().id] = 0; }
    }
    libraryVersion++; ++revisions.catalog;
    InvalidatePlaylistStatsCache();
    return &mediaItems.front();
}

MediaItem* MediaLibrary::AddOrUpdateMedia(const std::wstring& path, bool probeMetadata) {
    bool changed = false, probeFailed = false;
    MediaItem* m = AddOrUpdateMediaCore(path, changed, probeFailed, probeMetadata);
    if (changed) Save();
    return m;
}

MediaImportResult MediaLibrary::ImportPaths(const std::vector<std::wstring>& paths, const MediaImportProgressCallback& progress,
                                            bool saveChanges, ImportConflictPolicy conflictPolicy) {
    MediaImportResult result;
    result.inputCount = paths.size();
    std::vector<DiscoveredFile> files;
    std::set<std::wstring> seen;
    bool canceled = false;
    auto consider = [&](const std::wstring& candidate) {
        if (canceled || candidate.empty()) return;
        std::wstring resolved = ResolveAppPath(candidate);
        std::error_code ec;
        std::filesystem::path p(resolved);
        const bool exists = std::filesystem::exists(p, ec);
        if (ec || !exists) { result.skippedMissing++; return; }
        const bool regular = std::filesystem::is_regular_file(p, ec);
        if (ec || !regular) return;
        result.discoveredFiles++;
        if (progress && !progress(files.size(), 0, resolved)) { canceled = true; return; }
        if (!IsSupportedMediaExtension(resolved)) { result.skippedUnsupported++; return; }
        const std::wstring key = NormalizePathKey(resolved);
        if (key.empty() || !seen.insert(key).second) { result.skippedDuplicate++; return; }
        files.push_back({resolved, key});
    };
    for (const auto& input : paths) {
        if (canceled) break;
        if (input.empty()) { result.skippedMissing++; continue; }
        std::error_code ec;
        std::filesystem::path root(input);
        auto status = std::filesystem::symlink_status(root, ec);
        if (ec || status.type() == std::filesystem::file_type::not_found) { result.skippedMissing++; continue; }
        if (IsReparsePoint(input)) { result.skippedReparse++; continue; }
        if (std::filesystem::is_directory(status)) {
            std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end;
            for (; it != end && !ec && !canceled; it.increment(ec)) {
                std::error_code sec;
                auto entryStatus = it->symlink_status(sec);
                if (sec) continue;
                if (IsReparsePoint(it->path().wstring())) {
                    if (std::filesystem::is_directory(entryStatus)) it.disable_recursion_pending();
                    result.skippedReparse++;
                    continue;
                }
                if (std::filesystem::is_regular_file(entryStatus)) consider(it->path().wstring());
            }
            continue;
        }
        consider(input);
    }
    std::sort(files.begin(), files.end(),
              [](const DiscoveredFile &a, const DiscoveredFile &b) { return a.normalizedKey < b.normalizedKey; });
    if (progress && !progress(0, files.size(), L"")) canceled = true;
    std::vector<MediaItem> newItems;
    std::unordered_set<std::string> batchIds;
    bool anyChanged = false;
    for (size_t i = 0; i < files.size() && !canceled; ++i) {
        const auto& file = files[i];
        if (progress && !progress(i, files.size(), file.path)) { canceled = true; break; }
        MediaItem* existing = FindByPath(file.path);
        result.acceptedFiles++;
        result.acceptedPaths.push_back(file.path);
        const bool exists = FileExists(file.path);
        const uint64_t size = exists ? GetFileSizeInBytes(file.path) : 0;
        auto probe = [&](double& duration, uint32_t& w, uint32_t& h) {
            if (!ProbeMediaMetadata(file.path, duration, w, h)) { result.probeFailures++; return false; }
            duration = std::max(0.0, duration);
            return true;
        };
        if (existing) {
            result.conflictCount++;
            ImportConflictRecord conflictRec;
            conflictRec.path = file.path;
            conflictRec.existingId = existing->id;
            if (conflictPolicy == ImportConflictPolicy::Skip) {
                conflictRec.resolution = "skipped";
                result.conflictResolvedSkip++;
                result.unchanged++;
                result.conflicts.push_back(std::move(conflictRec));
                continue;
            } else if (conflictPolicy == ImportConflictPolicy::CreateCopy) {
                conflictRec.resolution = "created_copy";
                result.conflictResolvedCopy++;
                result.conflicts.push_back(std::move(conflictRec));
                MediaItem copyItem;
                std::string baseId = GenerateId(file.path);
                std::string newId = baseId + "_copy";
                for (unsigned k = 1; idToIndex.find(newId) != idToIndex.end() || batchIds.find(newId) != batchIds.end(); ++k) {
                    newId = baseId + "_copy_" + std::to_string(k);
                }
                batchIds.insert(newId);
                copyItem.id = newId;
                copyItem.filePath = file.path;
                copyItem.title = GetFileNameFromPath(file.path) + L" (Copy)";
                copyItem.fileSizeBytes = size;
                copyItem.addedTimestamp = GetCurrentTimestampSec();
                if (exists) {
                    double d = 0; uint32_t w = 0, h = 0;
                if (probe(d, w, h)) {
                    copyItem.durationSec = d;
                    copyItem.videoWidth = w;
                    copyItem.videoHeight = h;
                    copyItem.metadataProbeFailed = false;
                    copyItem.probeAttemptCount = 0;
                    copyItem.lastProbeAttemptTimestamp = 0;
                } else {
                    copyItem.metadataProbeFailed = true;
                    copyItem.probeAttemptCount = 1;
                    copyItem.lastProbeAttemptTimestamp = GetCurrentTimestampSec();
                }
                    WindowsFileIdentity fid = GetWindowsFileIdentity(file.path);
                    if (fid.valid) { copyItem.fileIndex = fid.fileIndex; copyItem.volumeSerialNumber = fid.volumeSerialNumber; }
                }
                newItems.push_back(std::move(copyItem));
                result.added++;
                anyChanged = true;
                continue;
            } else if (conflictPolicy == ImportConflictPolicy::Overwrite) {
                conflictRec.resolution = "overwritten";
                result.conflictResolvedOverwrite++;
                result.conflicts.push_back(std::move(conflictRec));
                existing->title = GetFileNameFromPath(file.path);
                existing->fileSizeBytes = size;
                double d = 0; uint32_t w = 0, h = 0;
                if (probe(d, w, h)) {
                    existing->durationSec = d;
                    existing->videoWidth = w;
                    existing->videoHeight = h;
                    existing->metadataProbeFailed = false;
                    existing->probeAttemptCount = 0;
                    existing->lastProbeAttemptTimestamp = 0;
                } else {
                    existing->metadataProbeFailed = true;
                    existing->probeAttemptCount++;
                    existing->lastProbeAttemptTimestamp = GetCurrentTimestampSec();
                }
                WindowsFileIdentity fid = GetWindowsFileIdentity(file.path);
                if (fid.valid) { existing->fileIndex = fid.fileIndex; existing->volumeSerialNumber = fid.volumeSerialNumber; }
                result.updated++;
                anyChanged = true;
            } else {
                conflictRec.resolution = "kept_existing";
                result.conflictResolvedKeep++;
                result.conflicts.push_back(std::move(conflictRec));
                bool itemChanged = false;
                if (exists && existing->fileSizeBytes != size) {
                    existing->fileSizeBytes = size;
                    itemChanged = true;
                    double d; uint32_t w, h;
                    if (probe(d, w, h)) {
                        existing->durationSec = d;
                        existing->videoWidth = w;
                        existing->videoHeight = h;
                        existing->metadataProbeFailed = false;
                        existing->probeAttemptCount = 0;
                        existing->lastProbeAttemptTimestamp = 0;
                    } else {
                        existing->metadataProbeFailed = true;
                        existing->probeAttemptCount++;
                        existing->lastProbeAttemptTimestamp = GetCurrentTimestampSec();
                    }
                    WindowsFileIdentity fid = GetWindowsFileIdentity(file.path);
                    if (fid.valid) { existing->fileIndex = fid.fileIndex; existing->volumeSerialNumber = fid.volumeSerialNumber; }
                } else if (exists && existing->durationSec <= 0) {
                    double d; uint32_t w, h;
                    if (probe(d, w, h)) {
                        existing->durationSec = d;
                        existing->videoWidth = w;
                        existing->videoHeight = h;
                        existing->metadataProbeFailed = false;
                        existing->probeAttemptCount = 0;
                        existing->lastProbeAttemptTimestamp = 0;
                        itemChanged = true;
                    } else {
                        existing->metadataProbeFailed = true;
                        existing->probeAttemptCount++;
                        existing->lastProbeAttemptTimestamp = GetCurrentTimestampSec();
                        itemChanged = true;
                    }
                    WindowsFileIdentity fid = GetWindowsFileIdentity(file.path);
                    if (fid.valid) { existing->fileIndex = fid.fileIndex; existing->volumeSerialNumber = fid.volumeSerialNumber; }
                }
                if (itemChanged) { result.updated++; anyChanged = true; } else { result.unchanged++; }
            }
        } else {
            MediaItem x;
            std::string id = GenerateId(file.path);
            if (batchIds.find(id) != batchIds.end()) {
                for (unsigned k = 1;; ++k) {
                    std::string cand = id + "_" + std::to_string(k);
                    if (idToIndex.find(cand) == idToIndex.end() && batchIds.find(cand) == batchIds.end()) { id = cand; break; }
                }
            }
            batchIds.insert(id);
            x.id = id;
            x.filePath = file.path;
            x.title = GetFileNameFromPath(file.path);
            x.fileSizeBytes = size;
            x.addedTimestamp = GetCurrentTimestampSec();
            if (exists) {
                double d = 0; uint32_t w = 0, h = 0;
                if (probe(d, w, h)) {
                    x.durationSec = d;
                    x.videoWidth = w;
                    x.videoHeight = h;
                    x.metadataProbeFailed = false;
                    x.probeAttemptCount = 0;
                    x.lastProbeAttemptTimestamp = 0;
                } else { x.metadataProbeFailed = true; x.probeAttemptCount = 1; x.lastProbeAttemptTimestamp = GetCurrentTimestampSec(); }
                WindowsFileIdentity fid = GetWindowsFileIdentity(file.path);
                if (fid.valid) { x.fileIndex = fid.fileIndex; x.volumeSerialNumber = fid.volumeSerialNumber; }
            }
            newItems.push_back(std::move(x));
            result.added++;
            anyChanged = true;
        }
    }
    if (!newItems.empty())
        mediaItems.insert(mediaItems.begin(), std::make_move_iterator(newItems.begin()), std::make_move_iterator(newItems.end()));
    if (anyChanged) { RebuildPathIndex(); libraryVersion++; InvalidatePlaylistStatsCache(); }
    if (progress && !canceled) progress(files.size(), files.size(), L"");
    result.outcome = canceled ? LongOperationOutcome::Canceled : LongOperationOutcome::Completed;
    if (saveChanges && anyChanged) {
        const MediaLibrarySaveResult saveResult = Save();
        if (!saveResult.is_ok()) {
            result.outcome = LongOperationOutcome::Failed;
            if (!saveResult.libraryStatus.is_ok()) result.status = saveResult.libraryStatus;
            else if (!saveResult.playlistsStatus.is_ok()) result.status = saveResult.playlistsStatus;
            else result.status = saveResult.historyStatus;
        }
    }
    return result;
}

bool MediaLibrary::RemoveMedia(const std::string &id) { return RemoveMedia(std::vector<std::string>{id}) != 0; }

size_t MediaLibrary::RemoveMedia(const std::vector<std::string>& ids, size_t* outSkipped, bool saveChanges,
                               std::vector<RemovedMediaInfo>* outRemoved) {
    if (outRemoved) outRemoved->clear();
    if (ids.empty() || mediaItems.empty()) { if (outSkipped) *outSkipped = ids.size(); return 0; }
    std::unordered_set<std::string> idsToRemove(ids.begin(), ids.end());
    if (outRemoved) outRemoved->reserve((std::min)(mediaItems.size(), idsToRemove.size()));
    std::unordered_set<std::wstring> removedPaths;
    removedPaths.reserve(idsToRemove.size());
    const size_t oldSize = mediaItems.size();
    mediaItems.erase(std::remove_if(mediaItems.begin(), mediaItems.end(), [&](const MediaItem& item) {
        if (idsToRemove.find(item.id) == idsToRemove.end()) return false;
        if (outRemoved) outRemoved->push_back({item.filePath, item.durationSec});
        removedPaths.insert(NormalizePathKey(item.filePath));
        return true;
    }), mediaItems.end());
    const size_t removedCount = oldSize - mediaItems.size();
    if (outSkipped) *outSkipped = (ids.size() >= removedCount) ? (ids.size() - removedCount) : 0;
    if (removedCount == 0) return 0;
    for (auto& playlist : playlists) {
        playlist.itemPaths.erase( std::remove_if(playlist.itemPaths.begin(), playlist.itemPaths.end(), [&](const std::wstring& path) {
                return removedPaths.find(NormalizePathKey(path)) != removedPaths.end();
            }), playlist.itemPaths.end());
    }
    RebuildPathIndex();
    libraryVersion++;
    InvalidatePlaylistStatsCache();
    if (saveChanges) Save();
    return removedCount;
}

void MediaLibrary::UpdateProgress(const std::wstring& path, double pos, double dur) {
    std::wstring resolved = ResolveAppPath(path);
    MediaItem* m = FindByPath(resolved);
    if (!m) m = AddOrUpdateMedia(resolved);
    if (!m) return;
    double oldDuration = m->durationSec;
    std::string oldStatus = m->status;
    const double oldPosSec = m->lastPosSec;
    const int64_t oldWatchedTimestamp = m->lastWatchedTimestamp;
    bool oldWatched = (oldDuration > 0.0 && (m->lastPosSec / oldDuration) >= 0.90) || (oldStatus == "done");
    if (std::isfinite(dur) && dur > 0) m->durationSec = dur;
    if (!std::isfinite(pos) || pos < 0) pos = 0;
    if (m->durationSec > 0) pos = std::min(pos, m->durationSec);
    m->lastPosSec = pos;
    m->lastWatchedTimestamp = GetCurrentTimestampSec();
    if (m->status != "done") { m->status = (m->durationSec > 0 && pos >= m->durationSec * 0.95) ? "done" : "in_progress"; }
    // History rows and cards render progress at second precision, so only
    // second-granular (or categorical) changes invalidate projections, not
    // every fractional position tick from the player.
    const bool visibleStateChanged = m->durationSec != oldDuration || m->status != oldStatus ||
        (std::floor(m->lastPosSec) != std::floor(oldPosSec)) ||
        m->lastWatchedTimestamp != oldWatchedTimestamp;
    if (visibleStateChanged) { libraryVersion++; ++revisions.playback; }
    if (m->durationSec != oldDuration) ++revisions.catalog;
    const std::wstring resolvedKey = NormalizePathKey(resolved);
    for (auto& record : history) {
        if (Identity(record.filePath) != resolvedKey) continue;
        if (visibleStateChanged) ++revisions.history;
        record.lastPosSec = m->lastPosSec;
        if (m->durationSec > 0.0) record.durationSec = m->durationSec;
        record.watchedTimestamp = m->lastWatchedTimestamp;
        break;
    }
    bool newWatched = (m->durationSec > 0.0 && (m->lastPosSec / m->durationSec) >= 0.90) || (m->status == "done");
    if (m->durationSec != oldDuration || m->status != oldStatus || newWatched != oldWatched) { InvalidatePlaylistStatsCache(false); }
}

bool MediaLibrary::SetWatchedCore(const std::wstring& path, bool watched) {
    if (path.empty()) return false;
    std::wstring resolved = ResolveAppPath(path);
    MediaItem* m = FindByPath(resolved);
    if (!m) m = AddOrUpdateMedia(resolved);
    if (!m) return false;
    if (watched) {
        m->status = "done";
        if (m->durationSec > 0.0) m->lastPosSec = m->durationSec;
        m->lastWatchedTimestamp = GetCurrentTimestampSec();
    } else { m->status = "new"; m->lastPosSec = 0.0; m->lastWatchedTimestamp = 0; }
    ++revisions.playback;
    InvalidatePlaylistStatsCache(false);
    return true;
}

void MediaLibrary::SetWatched(const std::wstring& path, bool watched) {
    if (SetWatchedCore(path, watched)) { libraryVersion++; Save(); }
}

size_t MediaLibrary::SetWatched(const std::vector<std::wstring>& paths, bool watched, size_t* outSkipped) {
    size_t completed = 0, skipped = 0;
    for (const auto& path : paths) {
        if (SetWatchedCore(path, watched)) { completed++; } else { skipped++; }
    }
    if (outSkipped) *outSkipped = skipped;
    if (completed > 0) { libraryVersion++; Save(); }
    return completed;
}

void MediaLibrary::AddToHistory(const std::wstring& path, double pos, double dur) {
    if (ClassifyPlaybackTarget(path) != PlaybackTargetKind::LocalFile) return;
    std::wstring resolved = ResolveAppPath(path);
    for (auto i = history.begin(); i != history.end(); ++i) {
        if (SamePath(i->filePath, resolved)) {
            HistoryRecord h = *i;
            history.erase(i);
            const auto newTitle = GetFileNameFromPath(resolved);
            if (h.title != newTitle || h.filePath != resolved) ++searchContentVersion;
            h.filePath = resolved;
            h.title = newTitle;
            h.lastPosSec = std::isfinite(pos) ? std::max(0.0, pos) : 0.0;
            h.durationSec = std::isfinite(dur) && dur > 0 ? dur : h.durationSec;
            h.watchedTimestamp = std::max<int64_t>(GetCurrentTimestampSec(), h.watchedTimestamp);
            history.insert(history.begin(), h);
            ++libraryVersion; ++revisions.history;
            Save();
            return;
        }
    }
    HistoryRecord h;
    h.filePath = resolved;
    h.title = GetFileNameFromPath(resolved);
    h.lastPosSec = std::isfinite(pos) ? std::max(0.0, pos) : 0.0;
    h.durationSec = std::isfinite(dur) && dur > 0 ? dur : 0;
    h.watchedTimestamp = GetCurrentTimestampSec();
    history.insert(history.begin(), h);
    if (history.size() > 100) history.resize(100);
    ++libraryVersion; ++revisions.history; ++searchContentVersion;
    Save();
}

bool MediaLibrary::RemoveFromHistory(size_t i) {
    if (i >= history.size()) return false;
    history.erase(history.begin() + i);
    ++libraryVersion; ++revisions.history; ++searchContentVersion;
    Save();
    return true;
}

void MediaLibrary::ClearHistory() {
    bool changed = !history.empty();
    if (changed) ++searchContentVersion;
    history.clear();
    for (auto& m : mediaItems) {
        if (m.lastPosSec > 0.0 || m.status != "new" || m.lastWatchedTimestamp != 0) {
            m.lastPosSec = 0.0;
            m.status = "new";
            m.lastWatchedTimestamp = 0;
            changed = true;
        }
    }
    if (changed) { libraryVersion++; ++revisions.playback; ++revisions.history; InvalidatePlaylistStatsCache(false); Save(); }
}

bool MediaLibrary::AddWatchedFolder(const std::wstring& path, bool saveChanges) {
    if (path.empty()) return false;
    std::wstring resolved = ResolveAppPath(path);
    std::wstring key = NormalizePathKey(resolved);
    if (key.empty()) return false;
    for (const auto& existing : watchedFolders) {
        if (NormalizePathKey(existing) == key) { return false; }
    }
    watchedFolders.push_back(resolved);
    libraryVersion++;
    if (saveChanges) Save();
    return true;
}

bool MediaLibrary::RemoveWatchedFolder(const std::wstring& path, bool saveChanges) {
    if (path.empty() || watchedFolders.empty()) return false;
    std::wstring key = NormalizePathKey(ResolveAppPath(path));
    if (key.empty()) return false;
    auto it =
        std::remove_if(watchedFolders.begin(), watchedFolders.end(), [&](const std::wstring &f) { return NormalizePathKey(f) == key; });
    if (it != watchedFolders.end()) {
        watchedFolders.erase(it, watchedFolders.end());
        libraryVersion++;
        if (saveChanges) Save();
        return true;
    }
    return false;
}

WatchReconcileResult MediaLibrary::ApplyPreparedWatchDelta(const PreparedWatchDelta& delta, bool saveChanges) {
    WatchReconcileResult result;
    if (delta.canceled) return result;
    result.rescanCount = delta.rescanCount;
    if (!delta.errors.empty()) result.status = delta.errors.front();
    if (delta.changes.empty()) return result;
    const size_t originalCount = mediaItems.size();
    const auto additions = static_cast<size_t>(std::count_if(delta.changes.begin(), delta.changes.end(),
        [](const auto& change) { return change.action == WatchAction::Added; }));
    mediaItems.reserve(originalCount + additions);
    std::unordered_map<std::string, size_t> liveIds;
    std::unordered_map<std::wstring, size_t> livePaths;
    liveIds.reserve(originalCount + delta.changes.size());
    livePaths.reserve(originalCount + delta.changes.size());
    for (size_t i = 0; i < originalCount; ++i) {
        liveIds[mediaItems[i].id] = i;
        livePaths[NormalizePathKey(mediaItems[i].filePath)] = i;
    }
    std::vector<std::wstring> rootKeys;
    for (const auto& root : watchedFolders) rootKeys.push_back(NormalizePathKey(root));
    std::unordered_set<size_t> removed;
    std::unordered_set<std::wstring> removedPaths;
    std::unordered_map<std::wstring, std::wstring> renamedPaths;
    bool structuralChange = false;
    for (const auto& change : delta.changes) {
        const auto pathKey = NormalizePathKey(change.path.empty() ? change.baseline.path : change.path);
        const auto baselineKey = NormalizePathKey(change.baseline.path);
        if (!std::any_of(rootKeys.begin(), rootKeys.end(), [&](const auto& root) {
                return WatchPathWithin(pathKey, root) || WatchPathWithin(baselineKey, root);
            })) continue; // The user may have removed this watch during the scan.
        size_t index = SIZE_MAX;
        if (!change.baseline.id.empty()) {
            const auto found = liveIds.find(change.baseline.id);
            if (found == liveIds.end() || removed.count(found->second)) continue;
            index = found->second;
            const auto& item = mediaItems[index];
            if (NormalizePathKey(item.filePath) != baselineKey || item.fileSizeBytes != change.baseline.size ||
                item.fileIndex != change.baseline.fileIndex || item.volumeSerialNumber != change.baseline.volumeSerialNumber) {
                result.needsRescan = true;
                continue;
            }
        } else {
            const auto found = livePaths.find(change.action != WatchAction::Added && !baselineKey.empty() ? baselineKey : pathKey);
            if (found != livePaths.end() && !removed.count(found->second)) {
                // A concurrent import owns this existing record. A repeated
                // observation of a record added in this transaction is safe.
                if (found->second < originalCount) continue;
                index = found->second;
            }
        }
        if (change.action == WatchAction::Removed) {
            if (index == SIZE_MAX) continue;
            removed.insert(index); removedPaths.insert(baselineKey); livePaths.erase(baselineKey);
            ++result.removedCount; structuralChange = true;
            continue;
        }
        if (index == SIZE_MAX) {
            if (change.action != WatchAction::Added) continue;
            MediaItem item;
            item.filePath = change.path; item.title = GetFileNameFromPath(change.path);
            item.addedTimestamp = GetCurrentTimestampSec(); item.fileSizeBytes = change.size;
            // Generate against the local transaction index; GenerateId would
            // otherwise rebuild the public index during a bulk insertion.
            uint64_t hash = 1469598103934665603ull;
            for (unsigned char c : Utf16ToUtf8(pathKey)) { hash ^= c; hash *= 1099511628211ull; }
            char buffer[48]; sprintf_s(buffer, "media_%016llx", static_cast<unsigned long long>(hash));
            const std::string baseId = buffer;
            item.id = baseId;
            for (size_t suffix = 1; liveIds.count(item.id); ++suffix) item.id = baseId + "_" + std::to_string(suffix);
            if (change.identity.valid) { item.fileIndex = change.identity.fileIndex; item.volumeSerialNumber = change.identity.volumeSerialNumber; }
            index = mediaItems.size();
            liveIds[item.id] = index; livePaths[pathKey] = index;
            mediaItems.push_back(std::move(item));
            ++result.addedCount; structuralChange = true;
            continue;
        }
        auto& item = mediaItems[index];
        if (change.action == WatchAction::Renamed) {
            const auto target = livePaths.find(pathKey);
            if (target != livePaths.end() && target->second != index && !removed.count(target->second)) {
                result.needsRescan = true; continue;
            }
            livePaths.erase(baselineKey); livePaths[pathKey] = index;
            renamedPaths[baselineKey] = change.path;
            item.filePath = change.path; item.title = GetFileNameFromPath(change.path);
            ++result.renamedCount; structuralChange = true;
        }
        const bool sizeChanged = item.fileSizeBytes != change.size;
        if (sizeChanged || change.metadataMayHaveChanged) {
            item.fileSizeBytes = change.size;
            // Retain current duration/resume information while the separate
            // metadata worker refreshes the changed file.
            item.metadataProbeFailed = true; item.probeAttemptCount = 0; item.lastProbeAttemptTimestamp = 0;
        }
        if (change.identity.valid) { item.fileIndex = change.identity.fileIndex; item.volumeSerialNumber = change.identity.volumeSerialNumber; }
        if (change.action != WatchAction::Renamed && (sizeChanged || change.metadataMayHaveChanged)) ++result.updatedCount;
    }
    if (result.addedCount + result.updatedCount + result.removedCount + result.renamedCount == 0) return result;
    if (!renamedPaths.empty() || !removedPaths.empty()) {
        auto resolve = [&](std::wstring path) {
            for (size_t i = 0; i < renamedPaths.size(); ++i) {
                const auto key = NormalizePathKey(path);
                const auto found = renamedPaths.find(key);
                if (found == renamedPaths.end()) break;
                path = found->second;
                if (NormalizePathKey(path) == key) break;
            }
            return path;
        };
        // A path removed and recreated in the same drain remains a valid
        // playlist/history reference, even though its media id changed.
        for (const auto& pair : livePaths) if (!removed.count(pair.second)) removedPaths.erase(pair.first);
        bool playlistsChanged = false, historyChanged = false;
        for (auto& playlist : playlists) {
            for (auto& path : playlist.itemPaths) {
                auto updated = resolve(path);
                if (updated != path) { path = std::move(updated); playlistsChanged = true; }
            }
            const auto end = std::remove_if(playlist.itemPaths.begin(), playlist.itemPaths.end(),
                [&](const auto& path) { return removedPaths.count(NormalizePathKey(path)) != 0; });
            playlistsChanged = playlistsChanged || end != playlist.itemPaths.end();
            playlist.itemPaths.erase(end, playlist.itemPaths.end());
        }
        for (auto& item : history) {
            auto updated = resolve(item.filePath);
            if (updated != item.filePath) { item.filePath = std::move(updated); item.title = GetFileNameFromPath(item.filePath); historyChanged = true; }
        }
        // Removing a library record does not erase the user's watch history.
        if (playlistsChanged) ++revisions.playlists;
        if (historyChanged) ++revisions.history;
    }
    if (structuralChange) {
        size_t write = 0, retainedOriginal = 0;
        for (size_t read = 0; read < mediaItems.size(); ++read) {
            if (removed.count(read)) continue;
            if (read < originalCount) ++retainedOriginal;
            if (write != read) mediaItems[write] = std::move(mediaItems[read]);
            ++write;
        }
        mediaItems.resize(write);
        // Match sequential front insertion, but move the existing catalog once.
        std::reverse(mediaItems.begin() + retainedOriginal, mediaItems.end());
        std::rotate(mediaItems.begin(), mediaItems.begin() + retainedOriginal, mediaItems.end());
        RebuildPathIndex();
    } else ++revisions.catalog;
    ++libraryVersion;
    InvalidatePlaylistStatsCache();
    if (saveChanges) Save();
    return result;
}

WatchReconcileResult MediaLibrary::ReconcileWatchBatch(const WatchChangeBatch& batch, bool saveChanges, bool probeMetadata) {
    WatchReconcileResult result;
    // Removals accumulate into one batched RemoveMedia call: the vector
    // overload rebuilds the path index and saves once for the whole batch.
    std::vector<std::string> batchedRemovedIds;
    if (batch.overflowOrRescan) { return ReconcileRescan(batch.rootFolder, saveChanges, probeMetadata); }
    bool libraryModified = false;
    for (const auto& item : batch.items) {
        switch (item.action) {
        case WatchAction::RescanRequired: {
            WatchReconcileResult subRes = ReconcileRescan(item.rootFolder, false, probeMetadata);
            result.addedCount += subRes.addedCount;
            result.updatedCount += subRes.updatedCount;
            result.removedCount += subRes.removedCount;
            result.renamedCount += subRes.renamedCount;
            result.rescanCount += subRes.rescanCount;
            if (!subRes.status.is_ok()) result.status = subRes.status;
            if (subRes.addedCount > 0 || subRes.updatedCount > 0 || subRes.removedCount > 0 || subRes.renamedCount > 0)
                libraryModified = true;
            break;
        }
        case WatchAction::Added: {
            if (item.path.empty()) break;
            std::wstring resolved = ResolveAppPath(item.path);
            if (!IsSupportedMediaExtension(resolved)) break;
            if (!FileExists(resolved)) break;
            bool changed = false, probeFailed = false;
            MediaItem* existing = FindByPath(resolved);
            if (!existing) {
                AddOrUpdateMediaCore(resolved, changed, probeFailed, probeMetadata);
                if (changed) { result.addedCount++; libraryModified = true; }
            } else {
                uint64_t size = GetFileSizeInBytes(resolved);
                if (existing->fileSizeBytes != size || existing->durationSec <= 0) {
                    AddOrUpdateMediaCore(resolved, changed, probeFailed, probeMetadata);
                    if (changed) { result.updatedCount++; libraryModified = true; }
                }
            }
            break;
        }
        case WatchAction::Removed: {
            if (item.path.empty()) break;
            std::wstring resolved = ResolveAppPath(item.path);
            MediaItem* mi = FindByPath(resolved);
            const std::wstring root = item.rootFolder.empty() ? batch.rootFolder : item.rootFolder;
            if ((!root.empty() && QueryPathAvailability(root) != PathAvailability::Present) ||
                QueryPathAvailability(resolved) != PathAvailability::Missing) break;
            if (mi) {
                batchedRemovedIds.push_back(mi->id);
            } else {
                std::wstring dirPrefix = resolved;
                if (!dirPrefix.empty() && dirPrefix.back() != L'\\' && dirPrefix.back() != L'/') { dirPrefix += L'\\'; }
                std::vector<std::string> idsToRemove;
                for (const auto& m : mediaItems) {
                    if (m.filePath.size() >= dirPrefix.size() && _wcsnicmp(m.filePath.c_str(), dirPrefix.c_str(), dirPrefix.size()) == 0)
                        idsToRemove.push_back(m.id);
                }
                if (!idsToRemove.empty()) { batchedRemovedIds.insert(batchedRemovedIds.end(), idsToRemove.begin(), idsToRemove.end()); }
            }
            break;
        }
        case WatchAction::Modified: {
            if (item.path.empty()) break;
            std::wstring resolved = ResolveAppPath(item.path);
            if (!IsSupportedMediaExtension(resolved)) break;
            if (!FileExists(resolved)) break;
            MediaItem* mi = FindByPath(resolved);
            if (mi) {
                uint64_t size = GetFileSizeInBytes(resolved);
                if (mi->fileSizeBytes != size || mi->durationSec <= 0) {
                    bool changed = false, probeFailed = false;
                    AddOrUpdateMediaCore(resolved, changed, probeFailed, probeMetadata);
                    if (changed) { result.updatedCount++; libraryModified = true; }
                }
            } else {
                bool changed = false, probeFailed = false;
                AddOrUpdateMediaCore(resolved, changed, probeFailed, probeMetadata);
                if (changed) { result.addedCount++; libraryModified = true; }
            }
            break;
        }
        case WatchAction::Renamed: {
            std::wstring oldResolved = ResolveAppPath(item.oldPath);
            std::wstring newResolved = ResolveAppPath(item.path);
            if (oldResolved.empty() && newResolved.empty()) break;
            MediaItem* mi = FindByPath(oldResolved);
            if (mi) {
                if (IsSupportedMediaExtension(newResolved) && FileExists(newResolved)) {
                    mi->filePath = newResolved;
                    mi->title = GetFileNameFromPath(newResolved);
                    mi->fileSizeBytes = GetFileSizeInBytes(newResolved);
                    WindowsFileIdentity fid = GetWindowsFileIdentity(newResolved);
                    if (fid.valid) { mi->fileIndex = fid.fileIndex; mi->volumeSerialNumber = fid.volumeSerialNumber; }
                    for (auto& pl : playlists) {
                        for (auto& p : pl.itemPaths) {
                            if (NormalizePathKey(p) == NormalizePathKey(oldResolved)) { p = newResolved; }
                        }
                    }
                    for (auto& hr : history) {
                        if (NormalizePathKey(hr.filePath) == NormalizePathKey(oldResolved)) {
                            hr.filePath = newResolved;
                            hr.title = mi->title;
                        }
                    }
                    RebuildPathIndex();
                    libraryVersion++;
                    InvalidatePlaylistStatsCache();
                    result.renamedCount++;
                    libraryModified = true;
                } else {
                    if (QueryPathAvailability(newResolved) == PathAvailability::Missing && RemoveMedia(std::vector<std::string>{mi->id}, nullptr, false)) {
                        result.removedCount++; libraryModified = true;
                    }
                }
            } else {
                std::wstring oldDirPrefix = oldResolved;
                if (!oldDirPrefix.empty() && oldDirPrefix.back() != L'\\' && oldDirPrefix.back() != L'/') { oldDirPrefix += L'\\'; }
                std::wstring newDirPrefix = newResolved;
                if (!newDirPrefix.empty() && newDirPrefix.back() != L'\\' && newDirPrefix.back() != L'/') { newDirPrefix += L'\\'; }
                bool anyDirectoryItemRenamed = false;
                for (auto& m : mediaItems) {
                    if (m.filePath.size() >= oldDirPrefix.size() &&
                        _wcsnicmp(m.filePath.c_str(), oldDirPrefix.c_str(), oldDirPrefix.size()) == 0) {
                        std::wstring sub = m.filePath.substr(oldDirPrefix.size());
                        std::wstring updatedPath = newDirPrefix + sub;
                        std::wstring oldPath = m.filePath;
                        m.filePath = updatedPath;
                        m.title = GetFileNameFromPath(updatedPath);
                        for (auto& pl : playlists) {
                            for (auto& p : pl.itemPaths) {
                                if (NormalizePathKey(p) == NormalizePathKey(oldPath)) { p = updatedPath; }
                            }
                        }
                        for (auto& hr : history) {
                            if (NormalizePathKey(hr.filePath) == NormalizePathKey(oldPath)) {
                                hr.filePath = updatedPath;
                                hr.title = m.title;
                            }
                        }
                        anyDirectoryItemRenamed = true;
                    }
                }
                if (anyDirectoryItemRenamed) {
                    RebuildPathIndex();
                    libraryVersion++;
                    InvalidatePlaylistStatsCache();
                    result.renamedCount++;
                    libraryModified = true;
                } else if (IsSupportedMediaExtension(newResolved) && FileExists(newResolved)) {
                    bool changed = false, probeFailed = false;
                    AddOrUpdateMediaCore(newResolved, changed, probeFailed, probeMetadata);
                    if (changed) { result.addedCount++; libraryModified = true; }
                }
            }
            break;
        }
        }
    }
    if (!batchedRemovedIds.empty()) {
        // One batched removal: a single path-index rebuild and a single save
        // instead of one full JSON save per deleted file.
        const size_t removed = RemoveMedia(batchedRemovedIds, nullptr, false);
        result.removedCount += removed;
        if (removed > 0) libraryModified = true;
    }
    if (libraryModified && saveChanges) { Save(); }
    return result;
}

WatchReconcileResult MediaLibrary::ReconcileRescan(const std::wstring& rootFolder, bool saveChanges, bool probeMetadata) {
    WatchReconcileResult result;
    result.rescanCount = 1;
    std::wstring resolvedRoot = ResolveAppPath(rootFolder);
    if (resolvedRoot.empty()) return result;
    DWORD attr = GetFileAttributesW(resolvedRoot.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        // Even PATH_NOT_FOUND can mean a disconnected volume/share. A rescan
        // may remove entries only after enumerating an available root.
        const DWORD err = attr == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY;
        result.status = StorageStatus::Error(StorageErrorCode::FileReadFailed, err, resolvedRoot, "Watched folder is unavailable; records retained");
        return result;
    }
    std::unordered_set<std::wstring> diskFiles;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(resolvedRoot, std::filesystem::directory_options::none, ec), end;
    bool changedAny = false;
    bool scanComplete = !ec;
    for (; it != end && !ec; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& entry = *it;
        std::error_code entryEc;
        if (entry.is_regular_file(entryEc)) {
            std::wstring filePath = entry.path().wstring();
            if (IsSupportedMediaExtension(filePath)) {
                std::wstring key = NormalizePathKey(filePath);
                diskFiles.insert(key);
                MediaItem* existing = FindByPath(filePath);
                if (!existing) {
                    bool changed = false, probeFailed = false;
                    AddOrUpdateMediaCore(filePath, changed, probeFailed, probeMetadata);
                    if (changed) { result.addedCount++; changedAny = true; }
                } else {
                    uint64_t size = GetFileSizeInBytes(filePath);
                    if (existing->fileSizeBytes != size || existing->durationSec <= 0) {
                        bool changed = false, probeFailed = false;
                        AddOrUpdateMediaCore(filePath, changed, probeFailed, probeMetadata);
                        if (changed) { result.updatedCount++; changedAny = true; }
                    }
                }
            }
        }
        if (entryEc) scanComplete = false;
    }
    const DWORD finalRootAttributes = GetFileAttributesW(resolvedRoot.c_str());
    scanComplete = scanComplete && !ec && finalRootAttributes != INVALID_FILE_ATTRIBUTES &&
        (finalRootAttributes & FILE_ATTRIBUTE_DIRECTORY);
    if (!scanComplete) {
        result.status = StorageStatus::Error(StorageErrorCode::FileReadFailed, ec ? static_cast<DWORD>(ec.value()) : ERROR_READ_FAULT,
            resolvedRoot, "Watched folder scan was incomplete; removals deferred");
        if (changedAny && saveChanges) Save();
        return result;
    }
    std::wstring dirPrefix = resolvedRoot;
    if (!dirPrefix.empty() && dirPrefix.back() != L'\\' && dirPrefix.back() != L'/') { dirPrefix += L'\\'; }
    std::vector<std::string> idsToRemove;
    for (const auto& m : mediaItems) {
        if (m.filePath.size() >= dirPrefix.size() && _wcsnicmp(m.filePath.c_str(), dirPrefix.c_str(), dirPrefix.size()) == 0) {
            std::wstring key = NormalizePathKey(m.filePath);
            if (diskFiles.find(key) == diskFiles.end() && QueryPathAvailability(m.filePath) == PathAvailability::Missing) {
                idsToRemove.push_back(m.id);
            }
        }
    }
    if (!idsToRemove.empty()) { size_t count = RemoveMedia(idsToRemove, nullptr, false); result.removedCount += count; changedAny = true; }
    if (changedAny && saveChanges) { Save(); }
    return result;
}

Playlist* MediaLibrary::CreatePlaylist(const std::string& name) {
    static uint64_t seq = 0;
    Playlist p;
    p.name = name.empty() ? "New Playlist" : name;
    p.createdTimestamp = GetCurrentTimestampSec();
    do { p.id = "pl_" + std::to_string(p.createdTimestamp) + "_" + std::to_string(++seq); } while (FindPlaylist(p.id));
    playlists.push_back(p);
    libraryVersion++; ++revisions.playlists;
    InvalidatePlaylistStatsCache();
    Save();
    return &playlists.back();
}

bool MediaLibrary::DeletePlaylist(const std::string& id) {
    auto i = std::remove_if(playlists.begin(), playlists.end(), [&](const Playlist& p) { return p.id == id; });
    if (i == playlists.end()) return false;
    playlists.erase(i, playlists.end());
    libraryVersion++; ++revisions.playlists;
    InvalidatePlaylistStatsCache();
    Save();
    return true;
}

bool MediaLibrary::AddToPlaylist(const std::string& id, const std::wstring& path) {
    return AddToPlaylist(id, std::vector<std::wstring>{ path }) > 0;
}

size_t MediaLibrary::AddToPlaylist(const std::string& id, const std::vector<std::wstring>& paths, size_t* outSkipped) {
    Playlist* p = FindPlaylist(id);
    if (!p || p->IsSmart() || paths.empty()) { if (outSkipped) *outSkipped = paths.size(); return 0; }
    std::unordered_set<std::wstring> existingPaths;
    existingPaths.reserve(p->itemPaths.size() + paths.size());
    for (const auto& existing : p->itemPaths) { existingPaths.insert(Identity(existing)); }
    size_t added = 0, skipped = 0;
    for (const auto& path : paths) {
        if (path.empty()) { skipped++; continue; }
        std::wstring resolved = ResolveAppPath(path);
        if (!existingPaths.insert(Identity(resolved)).second) { skipped++; continue; }
        p->itemPaths.push_back(resolved);
        AddOrUpdateMedia(resolved);
        added++;
    }
    if (outSkipped) *outSkipped = skipped;
    if (added > 0) { libraryVersion++; ++revisions.playlists; InvalidatePlaylistStatsCache(); Save(); }
    return added;
}

bool MediaLibrary::RemoveFromPlaylist(const std::string& id, size_t i) {
    Playlist* p = FindPlaylist(id);
    if (!p || p->IsSmart() || i >= p->itemPaths.size()) return false;
    p->itemPaths.erase(p->itemPaths.begin() + i);
    libraryVersion++; ++revisions.playlists;
    InvalidatePlaylistStatsCache();
    Save();
    return true;
}

Playlist* MediaLibrary::CreateSmartPlaylist(const std::string& name, const SmartPlaylistCriteria& criteria) {
    if (!criteria.Validate()) return nullptr;
    Playlist p;
    p.type = PlaylistType::Smart;
    p.name = name.empty() ? "Smart Playlist" : name;
    p.smartCriteria = criteria;
    p.createdTimestamp = GetCurrentTimestampSec();
    int count = 1;
    do { p.id = "pl_smart_" + std::to_string(p.createdTimestamp) + "_" + std::to_string(count++); } while (FindPlaylist(p.id));
    playlists.push_back(std::move(p));
    libraryVersion++; ++revisions.playlists;
    InvalidatePlaylistStatsCache();
    Save();
    return &playlists.back();
}

bool MediaLibrary::UpdateSmartPlaylist(const std::string& playlistId, const std::string& name, const SmartPlaylistCriteria& criteria) {
    if (!criteria.Validate()) return false;
    Playlist* p = FindPlaylist(playlistId);
    if (!p || !p->IsSmart()) return false;
    if (!name.empty()) { p->name = name; }
    p->smartCriteria = criteria;
    p->criteriaValidationError.clear();
    libraryVersion++; ++revisions.playlists;
    InvalidatePlaylistStatsCache();
    Save();
    return true;
}

std::vector<std::wstring> MediaLibrary::GetPlaylistPaths(const Playlist& playlist) const {
    return GetPlaylistPathsRef(playlist);
}

const std::vector<std::wstring>& MediaLibrary::GetPlaylistPathsRef(const Playlist& playlist) const {
    return ResolvePlaylistPaths(playlist).paths;
}

MediaLibrary::PlaylistPathsView MediaLibrary::ResolvePlaylistPaths(const Playlist& playlist) const {
    if (!playlist.IsSmart()) return { playlist.itemPaths, revisions.playlists };
    auto it = smartPathsCache.find(playlist.id);
    const int64_t nowSec = GetCurrentTimestampSec();
    if (it != smartPathsCache.end()) {
        const auto& entry = it->second;
        if (entry.revisions.catalog == revisions.catalog && entry.revisions.playlists == revisions.playlists &&
            (!entry.playbackDependent || entry.revisions.playback == revisions.playback) &&
            GetTickCount64() < entry.expiresAtTick &&
            (!entry.timeDependent || (nowSec >= entry.evaluatedAtSec && nowSec < entry.expiresAtSec)))
            return { entry.paths, entry.generation };
    }
    SmartPathsCacheEntry entry;
    const PreparedSmartPlaylist plan(playlist.smartCriteria, nowSec);
    entry.evaluatedAtSec = nowSec;
    entry.timeDependent = plan.DependsOnTime();
    entry.playbackDependent = plan.DependsOnPlayback();
    entry.paths = EvaluatePreparedSmartPlaylist(plan, entry.expiresAtTick, entry.expiresAtSec);
    entry.revisions = revisions;
    entry.generation = ++smartPathsGeneration;
    // Availability refreshes retire aggregates and UI row indices together.
    playlistStatsCache.erase(playlist.id);
    const auto& cached = smartPathsCache.insert_or_assign(playlist.id, std::move(entry)).first->second;
    return { cached.paths, cached.generation };
}

std::vector<std::wstring> MediaLibrary::GetPlaylistPaths(const std::string& playlistId) const {
    const Playlist* pl = FindPlaylist(playlistId);
    if (!pl) return {};
    return GetPlaylistPaths(*pl);
}

std::vector<std::wstring> MediaLibrary::EvaluateSmartPlaylist(const SmartPlaylistCriteria& criteria) const {
    const PreparedSmartPlaylist plan(criteria, GetCurrentTimestampSec());
    ULONGLONG expiresAtTick;
    int64_t expiresAtSec;
    return EvaluatePreparedSmartPlaylist(plan, expiresAtTick, expiresAtSec, false);
}

std::vector<std::wstring> MediaLibrary::EvaluatePreparedSmartPlaylist(const PreparedSmartPlaylist& plan,
    ULONGLONG& expiresAtTick, int64_t& expiresAtSec, bool useAvailabilityCache) const {
    expiresAtTick = (std::numeric_limits<ULONGLONG>::max)();
    expiresAtSec = (std::numeric_limits<int64_t>::max)();
    if (!plan.IsValid()) return {};
    ++smartPlaylistEvaluationCount;
    if (plan.RequiresAvailability() && useAvailabilityCache &&
        (smartAvailabilityCatalog != revisions.catalog || smartAvailability.size() != mediaItems.size())) {
        smartAvailability.assign(mediaItems.size(), SmartAvailabilityEntry{});
        smartAvailabilityCatalog = revisions.catalog;
    }
    std::vector<const MediaItem*> matched;
    matched.reserve(mediaItems.size());
    for (size_t i = 0; i < mediaItems.size(); ++i) {
        const auto& item = mediaItems[i];
        expiresAtSec = (std::min)(expiresAtSec, plan.NextChangeAfter(item));
        const auto possible = plan.MatchWithoutDisk(item);
        if (!possible.present && !possible.missing) continue;
        // Unavailable storage has unknown state, not a confirmed missing file.
        bool exists = true;
        if (plan.RequiresAvailability()) {
            SmartAvailabilityEntry uncached;
            auto& entry = useAvailabilityCache ? smartAvailability[i] : uncached;
            const ULONGLONG now = GetTickCount64();
            if (!entry.known || now - entry.checkedAtTick >= kSmartAvailabilityCacheTtlMs) {
                ++smartPlaylistDiskProbeCount;
                entry.value = QueryPathAvailability(item.filePath);
                entry.checkedAtTick = now;
                entry.known = true;
            }
            // Reusing another playlist's probe must not extend its deadline.
            expiresAtTick = (std::min)(expiresAtTick, entry.checkedAtTick + kSmartAvailabilityCacheTtlMs);
            if (entry.value == PathAvailability::Unavailable) continue;
            exists = entry.value == PathAvailability::Present;
        }
        if (exists ? possible.present : possible.missing) matched.push_back(&item);
    }
    auto sortField = static_cast<LibrarySortField>(plan.SortField());
    auto sortDir = static_cast<SortDirection>(plan.SortDirection());
    const auto less = [sortField, sortDir](const MediaItem* a, const MediaItem* b) {
        int cmp = LibraryQueryEngine::CompareItems(*a, *b, sortField, sortDir);
        if (cmp != 0) return cmp < 0;
        return a->filePath < b->filePath;
    };
    if (plan.Limit() > 0 && plan.Limit() < matched.size() / 4) {
        std::partial_sort(matched.begin(), matched.begin() + plan.Limit(), matched.end(),
            [sortField, sortDir](const MediaItem* a, const MediaItem* b) {
                const int cmp = LibraryQueryEngine::CompareItems(*a, *b, sortField, sortDir);
                if (cmp != 0) return cmp < 0;
                if (a->filePath != b->filePath) return a->filePath < b->filePath;
                // Both pointers belong to mediaItems: input order resolves
                // exact ties just as the unlimited stable sort does.
                return a < b;
            });
    } else {
        std::stable_sort(matched.begin(), matched.end(), less);
    }
    if (plan.Limit() > 0 && matched.size() > plan.Limit()) matched.resize(plan.Limit());
    std::vector<std::wstring> result;
    result.reserve(matched.size());
    for (const auto* item : matched) { result.push_back(item->filePath); }
    return result;
}

Playlist* MediaLibrary::FindPlaylist(const std::string& id) {
    for (auto& p : playlists) { if (p.id == id) return &p; }
    return nullptr;
}

const Playlist* MediaLibrary::FindPlaylist(const std::string& id) const {
    for (const auto& p : playlists) { if (p.id == id) return &p; }
    return nullptr;
}

size_t MediaLibrary::GetPlaylistResumeIndex(const Playlist* playlist) const {
    if (!playlist) return 0;
    return GetPlaylistResumeIndex(GetPlaylistPathsRef(*playlist));
}

size_t MediaLibrary::GetPlaylistResumeIndex(const std::vector<std::wstring>& paths) const {
    if (paths.empty()) return 0;
    int firstUnfinishedIndex = -1;
    for (size_t i = 0; i < paths.size(); ++i) {
        const MediaItem* item = FindByPath(paths[i]);
        if (!item) { if (firstUnfinishedIndex < 0) firstUnfinishedIndex = (int)i; continue; }
        if (item->status == "in_progress") { return i; }
        if (item->status == "new" && firstUnfinishedIndex < 0) { firstUnfinishedIndex = (int)i; }
    }
    if (firstUnfinishedIndex >= 0) { return (size_t)firstUnfinishedIndex; }
    return 0;
}

StorageStatus MediaLibrary::Load() { return LoadFromRoot(GetExeDir()); }

StorageStatus MediaLibrary::LoadFromRoot(const std::wstring& requestedRootDir, const LibraryLoadMilestoneCallback& milestone) {
    lastSaveSnapshot.reset();
    saveState = {};
    portableRoot = requestedRootDir.empty() ? GetExeDir() : requestedRootDir;
    // A missing store in an unavailable portable root is not a fresh profile.
    const DWORD rootAttributes = GetFileAttributesW(portableRoot.c_str());
    const DWORD rootError = rootAttributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY;
    lastLoadResult = {};
    lastLoadResult.success = false;
    lastLoadStatus = StorageStatus::Error(StorageErrorCode::FileReadFailed, rootError, portableRoot,
                                         "Portable library root is unavailable or load did not complete");
    lastLoadResult.libraryStatus = lastLoadResult.playlistsStatus = lastLoadResult.historyStatus =
        lastLoadResult.watchedFoldersStatus = lastLoadStatus;
    if (rootAttributes == INVALID_FILE_ATTRIBUTES || !(rootAttributes & FILE_ATTRIBUTE_DIRECTORY)) return lastLoadStatus;
    mediaItems.clear();
    playlists.clear();
    history.clear();
    pathToIndex.clear();
    idToIndex.clear();
    InvalidatePlaylistStatsCache();
    MediaLibraryLoadResult loadResult;
    const std::wstring rootDir = requestedRootDir.empty() ? GetExeDir() : requestedRootDir;
    const std::wstring dataDir = rootDir + L"\\data";
    auto loadStore = [&](const std::wstring& filePath, JsonValue& outRoot, bool& outRecovered, const char* collection, int currentVersion) -> StorageStatus {
        outRecovered = false;
        DWORD pathError = ERROR_SUCCESS;
        const auto availability = QueryPathAvailability(filePath, &pathError);
        if (availability == PathAvailability::Missing) return StorageStatus::Ok(filePath);
        if (availability == PathAvailability::Unavailable)
            return StorageStatus::Error(StorageErrorCode::FileReadFailed, pathError, filePath, "Store is unavailable");
        const auto valid = [collection, currentVersion](const JsonValue& root) {
            if (root.is_array()) return true; // Supported legacy form.
            if (!root.is_object() || !root.get(collection).is_array()) return false;
            const auto& version = root.get("schemaVersion");
            if (!root.has_key("schemaVersion")) return true;
            const double number = version.as_number(-1);
            return version.is_number() && std::isfinite(number) && number >= 0 && number <= currentVersion && std::floor(number) == number;
        };
        std::string text;
        StorageStatus readRes = ReadTextFile(filePath, text);
        if (readRes.is_ok() && JsonValue::parse(text, outRoot)) {
            if (outRoot.is_object() && outRoot.get("schemaVersion").as_number(0) > currentVersion)
                return StorageStatus::Error(StorageErrorCode::SerializationFailed, ERROR_REVISION_MISMATCH, filePath, "Store requires a newer application version");
            if (valid(outRoot)) return StorageStatus::Ok(filePath);
        }
        std::string backupText;
        StorageStatus backupRes = RestoreFromNewestValidBackup(filePath, backupText, [&](const std::string& t) {
            JsonValue j;
            return JsonValue::parse(t, j) && valid(j);
        });
        if (backupRes.is_ok() && JsonValue::parse(backupText, outRoot) && valid(outRoot)) {
            outRecovered = true;
            return StorageStatus::Ok(filePath);
        }
        return StorageStatus::Error(StorageErrorCode::SerializationFailed, ERROR_INVALID_DATA, filePath, "Malformed JSON and no valid backup");
    };
    // Read and syntax-validate every portable store before constructing typed state. This creates a clean measurement boundary and ensures
    // the worker never publishes a half-read snapshot.
    std::wstring libPath = dataDir + L"\\library.json";
    std::wstring plPath = dataDir + L"\\playlists.json";
    std::wstring histPath = dataDir + L"\\history.json";
    std::wstring watchPath = dataDir + L"\\watched_folders.json";
    JsonValue libRoot;
    JsonValue plRoot;
    JsonValue histRoot;
    JsonValue watchRoot;
    loadResult.libraryStatus = loadStore(libPath, libRoot, loadResult.libraryRecoveredFromBackup, "items", kCurrentLibrarySchemaVersion);
    loadResult.playlistsStatus = loadStore(plPath, plRoot, loadResult.playlistsRecoveredFromBackup, "items", kCurrentPlaylistsSchemaVersion);
    loadResult.historyStatus = loadStore(histPath, histRoot, loadResult.historyRecoveredFromBackup, "items", kCurrentHistorySchemaVersion);
    loadResult.watchedFoldersStatus = loadStore(watchPath, watchRoot, loadResult.watchedFoldersRecoveredFromBackup, "folders", kCurrentWatchedFoldersSchemaVersion);
    if (milestone) milestone(L"library-files-read");
    // 1. Library
    if (loadResult.libraryStatus.is_ok()) {
        int ver = libRoot.is_object() ? libRoot.get("schemaVersion").as_int(1) : 1;
        MigrateLibrarySchema(libRoot, ver, kCurrentLibrarySchemaVersion);
        JsonValue* items = libRoot.is_object() && libRoot.get("items").is_array() ? &libRoot["items"] : (libRoot.is_array() ? &libRoot : nullptr);
        if (items && items->is_array()) {
            mediaItems.reserve(items->as_array().size());
            pathToIndex.reserve(items->as_array().size());
            idToIndex.reserve(items->as_array().size());
            for (auto& json : items->as_array()) {
                if (!json.is_object()) continue;
                MediaItem item;
                item.id = json.get("id").as_string();
                std::wstring rawPath = json.get("filePath").as_wstring();
                if (ClassifyPlaybackTarget(rawPath) != PlaybackTargetKind::LocalFile) continue;
                item.filePath = ResolveAppPath(rawPath, rootDir);
                if (item.filePath.empty()) continue;
                item.title = json.get("title").as_wstring();
                if (item.title.empty()) item.title = GetFileNameFromPath(item.filePath);
                item.durationSec = std::max(0.0, SafeNumber(json.get("durationSec")));
                item.lastPosSec = std::max(0.0, SafeNumber(json.get("lastPosSec")));
                item.status = json.get("status").as_string("new");
                if (item.status != "new" && item.status != "in_progress" && item.status != "done") item.status = "new";
                item.fileSizeBytes = json.get("fileSizeBytes").as_uint64(0);
                item.lastWatchedTimestamp = SafeTime(json.get("lastWatchedTimestamp"));
                item.addedTimestamp = SafeTime(json.get("addedTimestamp"));
                item.videoWidth = (uint32_t)std::max(0, json.get("videoWidth").as_int(0));
                item.videoHeight = (uint32_t)std::max(0, json.get("videoHeight").as_int(0));
                item.fileIndex = ExactUint64(json.get("fileIndex"));
                item.volumeSerialNumber = (uint32_t)json.get("volumeSerialNumber").as_uint64(0);
                item.metadataProbeFailed = json.get("metadataProbeFailed").as_bool(false);
                item.probeAttemptCount = (uint32_t)std::max(0, json.get("probeAttemptCount").as_int(0));
                item.lastProbeAttemptTimestamp = SafeTime(json.get("lastProbeAttemptTimestamp"));
                if (item.durationSec > 0) item.lastPosSec = std::min(item.lastPosSec, item.durationSec);
                if (json.has_key("preferences") && json.get("preferences").is_object()) {
                    const auto& prefJson = json.get("preferences");
                    MediaPlaybackPreferences prefs;
                    if (prefJson.has_key("playbackSpeed") && prefJson.get("playbackSpeed").is_number()) {
                        double spd = SafeNumber(prefJson.get("playbackSpeed"));
                        if (MediaPlaybackPreferences::IsValidPlaybackSpeed(spd)) { prefs.playbackSpeed = spd; }
                    }
                    if (prefJson.has_key("aspectRatio") && prefJson.get("aspectRatio").is_string()) {
                        std::string ar = prefJson.get("aspectRatio").as_string();
                        if (MediaPlaybackPreferences::IsValidAspectRatio(ar)) { prefs.aspectRatio = ar; }
                    }
                    if (prefJson.has_key("audioTrackId") && prefJson.get("audioTrackId").is_number()) {
                        int atId = prefJson.get("audioTrackId").as_int();
                        if (atId >= 0) prefs.audioTrackId = atId;
                    }
                    if (prefJson.has_key("audioTrackTitle") && prefJson.get("audioTrackTitle").is_string())
                        prefs.audioTrackTitle = prefJson.get("audioTrackTitle").as_wstring();
                    if (prefJson.has_key("audioTrackLang") && prefJson.get("audioTrackLang").is_string())
                        prefs.audioTrackLang = prefJson.get("audioTrackLang").as_string();
                    if (prefJson.has_key("subtitleTrackId") && prefJson.get("subtitleTrackId").is_number()) {
                        int stId = prefJson.get("subtitleTrackId").as_int();
                        if (stId >= 0) prefs.subtitleTrackId = stId;
                    }
                    if (prefJson.has_key("subtitleTrackTitle") && prefJson.get("subtitleTrackTitle").is_string())
                        prefs.subtitleTrackTitle = prefJson.get("subtitleTrackTitle").as_wstring();
                    if (prefJson.has_key("subtitleTrackLang") && prefJson.get("subtitleTrackLang").is_string())
                        prefs.subtitleTrackLang = prefJson.get("subtitleTrackLang").as_string();
                    if (prefJson.has_key("audioDelayMs") && prefJson.get("audioDelayMs").is_number()) {
                        int delay = prefJson.get("audioDelayMs").as_int();
                        if (MediaPlaybackPreferences::IsValidAudioDelayMs(delay)) { prefs.audioDelayMs = delay; }
                    }
                    if (prefJson.has_key("subDelayMs") && prefJson.get("subDelayMs").is_number()) {
                        int delay = prefJson.get("subDelayMs").as_int();
                        if (MediaPlaybackPreferences::IsValidSubDelayMs(delay)) { prefs.subDelayMs = delay; }
                    }
                    if (prefJson.has_key("subtitlesVisible") && prefJson.get("subtitlesVisible").is_bool())
                        prefs.subtitlesVisible = prefJson.get("subtitlesVisible").as_bool();
                    if (prefJson.has_key("videoRotate") && prefJson.get("videoRotate").is_number()) {
                        int r = prefJson.get("videoRotate").as_int();
                        if (MediaPlaybackPreferences::IsValidVideoRotate(r)) { prefs.videoRotate = r; }
                    }
                    if (prefJson.has_key("brightness") && prefJson.get("brightness").is_number()) {
                        double val = SafeNumber(prefJson.get("brightness"), 999.0);
                        if (std::isfinite(val) && MediaPlaybackPreferences::IsValidBrightness((int)std::round(val)))
                            prefs.brightness = (int)std::round(val);
                    }
                    if (prefJson.has_key("contrast") && prefJson.get("contrast").is_number()) {
                        double val = SafeNumber(prefJson.get("contrast"), 999.0);
                        if (std::isfinite(val) && MediaPlaybackPreferences::IsValidContrast((int)std::round(val)))
                            prefs.contrast = (int)std::round(val);
                    }
                    if (prefJson.has_key("saturation") && prefJson.get("saturation").is_number()) {
                        double val = SafeNumber(prefJson.get("saturation"), 999.0);
                        if (std::isfinite(val) && MediaPlaybackPreferences::IsValidSaturation((int)std::round(val)))
                            prefs.saturation = (int)std::round(val);
                    }
                    if (prefJson.has_key("gamma") && prefJson.get("gamma").is_number()) {
                        double val = SafeNumber(prefJson.get("gamma"), 999.0);
                        if (std::isfinite(val) && MediaPlaybackPreferences::IsValidGamma((int)std::round(val)))
                            prefs.gamma = (int)std::round(val);
                    }
                    if (prefJson.has_key("hue") && prefJson.get("hue").is_number()) {
                        double val = SafeNumber(prefJson.get("hue"), 999.0);
                        if (std::isfinite(val) && MediaPlaybackPreferences::IsValidHue((int)std::round(val)))
                            prefs.hue = (int)std::round(val);
                    }
                    if (prefJson.has_key("audioBalance") && prefJson.get("audioBalance").is_number()) {
                        double b = SafeNumber(prefJson.get("audioBalance"), 999.0);
                        if (MediaPlaybackPreferences::IsValidAudioBalance(b)) { prefs.audioBalance = b; }
                    }
                    if (prefJson.has_key("audioChannelMap") && prefJson.get("audioChannelMap").is_string()) {
                        std::string rawMap = prefJson.get("audioChannelMap").as_string();
                        std::string normMap = AudioConfig::NormalizeAudioChannelMap(rawMap);
                        if (MediaPlaybackPreferences::IsValidAudioChannelMap(normMap)) { prefs.audioChannelMap = normMap; }
                    }
                    {
                        JsonValue sparse = prefJson;
                        static constexpr const char* kKnownPrefKeys[] = {
                            "playbackSpeed", "aspectRatio", "audioTrackId", "audioTrackTitle",
                            "audioTrackLang", "subtitleTrackId", "subtitleTrackTitle", "subtitleTrackLang",
                            "audioDelayMs", "subDelayMs", "subtitlesVisible", "videoRotate",
                            "brightness", "contrast", "saturation", "gamma", "hue", "audioBalance", "audioChannelMap"
                        };
                        StripKnownKeys(sparse, kKnownPrefKeys, sizeof(kKnownPrefKeys) / sizeof(kKnownPrefKeys[0]));
                        prefs.extraFields = std::move(sparse);
                    }
                    item.preferences = std::move(prefs);
                }
                if (json.has_key("bookmarks") && json.get("bookmarks").is_array()) {
                    const auto& bmarks = json.get("bookmarks").as_array();
                    for (const auto& bJson : bmarks) {
                        if (!bJson.is_object()) continue;
                        MediaBookmark bm;
                        bm.id = bJson.get("id").as_string();
                        bm.timeSec = SafeNumber(bJson.get("timeSec"), -1.0);
                        if (!MediaBookmark::IsValidTime(bm.timeSec)) continue;
                        bm.label = bJson.get("label").as_wstring();
                        if (bm.label.length() > MediaBookmark::kMaxLabelLength)
                            bm.label = bm.label.substr(0, MediaBookmark::kMaxLabelLength);
                        bm.createdTimestamp = SafeTime(bJson.get("createdTimestamp"));
                        if (bm.id.empty())
                            bm.id = GenerateId(item.filePath + L"_" + std::to_wstring(bm.timeSec) + L"_" + std::to_wstring(bm.createdTimestamp));
                        {
                            JsonValue sparse = bJson;
                            static constexpr const char* kKnownBookmarkKeys[] = { "id", "timeSec", "label", "createdTimestamp" };
                            StripKnownKeys(sparse, kKnownBookmarkKeys, sizeof(kKnownBookmarkKeys) / sizeof(kKnownBookmarkKeys[0]));
                            bm.extraFields = std::move(sparse);
                        }
                        item.bookmarks.push_back(std::move(bm));
                    }
                    std::stable_sort(item.bookmarks.begin(), item.bookmarks.end(), MediaBookmark::Compare);
                }
                if (json.has_key("overrides") && json.get("overrides").is_object()) {
                    const auto& ovJson = json.get("overrides");
                    MediaMetadataOverrides ov;
                    if (ovJson.has_key("title") && ovJson.get("title").is_string()) {
                        std::wstring t = ovJson.get("title").as_wstring();
                        if (MediaMetadataOverrides::IsValidTitle(t)) ov.title = t;
                    }
                    if (ovJson.has_key("year") && ovJson.get("year").is_number()) {
                        int y = ovJson.get("year").as_int();
                        if (MediaMetadataOverrides::IsValidYear(y)) ov.year = y;
                    }
                    if (ovJson.has_key("series") && ovJson.get("series").is_string()) {
                        std::wstring s = ovJson.get("series").as_wstring();
                        if (MediaMetadataOverrides::IsValidSeries(s)) ov.series = s;
                    }
                    {
                        JsonValue sparse = ovJson;
                        static constexpr const char* kKnownOverrideKeys[] = { "title", "year", "series" };
                        StripKnownKeys(sparse, kKnownOverrideKeys, sizeof(kKnownOverrideKeys) / sizeof(kKnownOverrideKeys[0]));
                        ov.extraFields = std::move(sparse);
                    }
                    if (ov.HasAnyOverride()) { item.overrides = std::move(ov); }
                } else if (json.has_key("metadataOverrides") && json.get("metadataOverrides").is_object()) {
                    const auto& ovJson = json.get("metadataOverrides");
                    MediaMetadataOverrides ov;
                    if (ovJson.has_key("title") && ovJson.get("title").is_string()) {
                        std::wstring t = ovJson.get("title").as_wstring();
                        if (MediaMetadataOverrides::IsValidTitle(t)) ov.title = t;
                    }
                    if (ovJson.has_key("year") && ovJson.get("year").is_number()) {
                        int y = ovJson.get("year").as_int();
                        if (MediaMetadataOverrides::IsValidYear(y)) ov.year = y;
                    }
                    if (ovJson.has_key("series") && ovJson.get("series").is_string()) {
                        std::wstring s = ovJson.get("series").as_wstring();
                        if (MediaMetadataOverrides::IsValidSeries(s)) ov.series = s;
                    }
                    {
                        JsonValue sparse = ovJson;
                        static constexpr const char* kKnownOverrideKeys[] = { "title", "year", "series" };
                        StripKnownKeys(sparse, kKnownOverrideKeys, sizeof(kKnownOverrideKeys) / sizeof(kKnownOverrideKeys[0]));
                        ov.extraFields = std::move(sparse);
                    }
                    if (ov.HasAnyOverride()) { item.overrides = std::move(ov); }
                }
                const std::wstring pathKey = NormalizePathKey(item.filePath);
                if (pathKey.empty() || pathToIndex.find(pathKey) != pathToIndex.end()) continue;
                if (item.id.empty()) item.id = GenerateId(item.filePath);
                {
                    static constexpr const char* kKnownMediaKeys[] = {
                        "id", "filePath", "title", "durationSec", "lastPosSec", "status",
                        "fileSizeBytes", "lastWatchedTimestamp", "addedTimestamp",
                        "videoWidth", "videoHeight", "fileIndex", "volumeSerialNumber",
                        "metadataProbeFailed", "probeAttemptCount", "lastProbeAttemptTimestamp",
                        "preferences", "bookmarks", "overrides", "metadataOverrides"
                    };
                    StripKnownKeys(json, kKnownMediaKeys, sizeof(kKnownMediaKeys) / sizeof(kKnownMediaKeys[0]));
                    item.extraFields = std::move(json);
                }
                const size_t index = mediaItems.size();
                mediaItems.push_back(std::move(item));
                pathToIndex.emplace(pathKey, index);
                idToIndex[mediaItems.back().id] = index;
            }
        }
    }
    // 2. Playlists
    if (loadResult.playlistsStatus.is_ok()) {
        int ver = plRoot.is_object() ? plRoot.get("schemaVersion").as_int(1) : 1;
        MigratePlaylistsSchema(plRoot, ver, kCurrentPlaylistsSchemaVersion);
        JsonValue* items = plRoot.is_object() && plRoot.get("items").is_array() ? &plRoot["items"] : (plRoot.is_array() ? &plRoot : nullptr);
        if (items && items->is_array()) {
            playlists.reserve(items->as_array().size());
            std::unordered_set<std::string> playlistIds;
            playlistIds.reserve(items->as_array().size());
            for (auto& json : items->as_array()) {
                if (!json.is_object()) continue;
                Playlist playlist;
                playlist.id = json.get("id").as_string();
                playlist.name = json.get("name").as_string("Playlist");
                playlist.createdTimestamp = SafeTime(json.get("createdTimestamp"));
                if (playlist.id.empty())
                    playlist.id = "pl_" + std::to_string(playlist.createdTimestamp) + "_" + std::to_string(playlists.size() + 1);
                if (!playlistIds.insert(playlist.id).second) {
                    playlist.id += "_" + std::to_string(playlists.size() + 1);
                    playlistIds.insert(playlist.id);
                }
                std::string typeStr = json.get("type").as_string();
                bool isSmart = json.get("isSmart").as_bool(false) || (typeStr == "smart") || json.has_key("smartCriteria") || json.has_key("rules");
                if (isSmart) {
                    playlist.type = PlaylistType::Smart;
                    if (json.has_key("smartCriteria")) {
                        playlist.smartCriteria = SmartPlaylistCriteria::FromJson(json.get("smartCriteria"));
                    } else { playlist.smartCriteria = SmartPlaylistCriteria::FromJson(json); }
                } else {
                    playlist.type = PlaylistType::Manual;
                    if (json.get("itemPaths").is_array()) {
                        playlist.itemPaths.reserve(json.get("itemPaths").as_array().size());
                        std::unordered_set<std::wstring> seenPaths;
                        seenPaths.reserve(json.get("itemPaths").as_array().size());
                        for (const auto& value : json.get("itemPaths").as_array()) {
                            std::wstring rawPath = value.as_wstring();
                            if (rawPath.empty()) continue;
                            std::wstring path = ResolveAppPath(rawPath, rootDir);
                            if (seenPaths.insert(NormalizePathKey(path)).second) { playlist.itemPaths.push_back(std::move(path)); }
                        }
                    }
                }
                if (isSmart) playlist.smartCriteria.Validate(&playlist.criteriaValidationError);
                {
                    static constexpr const char* kKnownPlaylistKeys[] = {
                        "id", "name", "type", "isSmart", "itemPaths", "smartCriteria", "createdTimestamp"};
                    StripKnownKeys(json, kKnownPlaylistKeys, sizeof(kKnownPlaylistKeys) / sizeof(kKnownPlaylistKeys[0]));
                    if (isSmart) {
                        // Top-level smart-criteria form (legacy): these keys are typed state via SmartPlaylistCriteria::FromJson(json).
                        // Save() emits the nested "smartCriteria" form, so
                        // strip them here to keep one authoritative copy.
                        static constexpr const char* kKnownSmartTopKeys[] = { "matchMode", "sortField", "sortDirection", "limit", "rules" };
                        // Only strip "rules" for smart playlists; manual lists
                        // keep an unknown "rules" key untouched.
                        StripKnownKeys(json, kKnownSmartTopKeys, sizeof(kKnownSmartTopKeys) / sizeof(kKnownSmartTopKeys[0]));
                    }
                    playlist.extraFields = std::move(json);
                }
                playlists.push_back(std::move(playlist));
            }
        }
    }
    if (playlists.empty()) {
        Playlist playlist;
        playlist.id = "pl_favorites";
        playlist.name = "Favorites";
        playlist.createdTimestamp = GetCurrentTimestampSec();
        playlists.push_back(std::move(playlist));
    }
    // 3. History
    if (loadResult.historyStatus.is_ok()) {
        int ver = histRoot.is_object() ? histRoot.get("schemaVersion").as_int(1) : 1;
        MigrateHistorySchema(histRoot, ver, kCurrentHistorySchemaVersion);
        JsonValue* items = histRoot.is_object() && histRoot.get("items").is_array() ? &histRoot["items"] : (histRoot.is_array() ? &histRoot : nullptr);
        if (items && items->is_array()) {
            history.reserve(std::min<size_t>(items->as_array().size(), 100));
            std::unordered_set<std::wstring> seenPaths;
            seenPaths.reserve(items->as_array().size());
            for (auto& json : items->as_array()) {
                if (!json.is_object()) continue;
                HistoryRecord record;
                std::wstring rawPath = json.get("filePath").as_wstring();
                if (ClassifyPlaybackTarget(rawPath) != PlaybackTargetKind::LocalFile) continue;
                record.filePath = ResolveAppPath(rawPath, rootDir);
                if (record.filePath.empty() || !seenPaths.insert(NormalizePathKey(record.filePath)).second) continue;
                record.title = json.get("title").as_wstring();
                if (record.title.empty()) record.title = GetFileNameFromPath(record.filePath);
                record.lastPosSec = std::max(0.0, SafeNumber(json.get("lastPosSec")));
                record.durationSec = std::max(0.0, SafeNumber(json.get("durationSec")));
                if (record.durationSec > 0) record.lastPosSec = std::min(record.lastPosSec, record.durationSec);
                record.watchedTimestamp = SafeTime(json.get("watchedTimestamp"));
                {
                    static constexpr const char *kKnownHistoryKeys[] = {"filePath", "title", "lastPosSec", "durationSec",
                                                                        "watchedTimestamp"};
                    StripKnownKeys(json, kKnownHistoryKeys, sizeof(kKnownHistoryKeys) / sizeof(kKnownHistoryKeys[0]));
                    record.extraFields = std::move(json);
                }
                history.push_back(std::move(record));
            }
        }
    }
    std::stable_sort(history.begin(), history.end(), [](const HistoryRecord& a, const HistoryRecord& b) {
        if (a.watchedTimestamp != b.watchedTimestamp) return a.watchedTimestamp > b.watchedTimestamp;
        return Identity(a.filePath) < Identity(b.filePath);
    });
    if (history.size() > 100) history.resize(100);
    // 4. Watched Folders
    watchedFolders.clear();
    if (loadResult.watchedFoldersStatus.is_ok()) {
        int ver = watchRoot.is_object() ? watchRoot.get("schemaVersion").as_int(1) : 1;
        MigrateWatchedFoldersSchema(watchRoot, ver, kCurrentWatchedFoldersSchemaVersion);
        JsonValue* foldersJson = watchRoot.is_object() && watchRoot.get("folders").is_array() ? &watchRoot["folders"] : (watchRoot.is_array() ? &watchRoot : nullptr);
        if (foldersJson && foldersJson->is_array()) {
            std::unordered_set<std::wstring> seenFolders;
            for (auto& json : foldersJson->as_array()) {
                std::wstring rawPath = json.as_wstring();
                if (rawPath.empty()) continue;
                std::wstring resolved = ResolveAppPath(rawPath, rootDir);
                std::wstring key = NormalizePathKey(resolved);
                if (!key.empty() && seenFolders.insert(key).second) { watchedFolders.push_back(std::move(resolved)); }
            }
        }
    }
    if (milestone) milestone(L"library-json-parsed");
    libraryVersion++; ++revisions.catalog; ++revisions.playback; ++revisions.playlists; ++revisions.history;
    InvalidatePlaylistStatsCache();
    lastLoadResult = loadResult;
    ++searchContentVersion;
    if (!loadResult.libraryStatus.is_ok()) lastLoadStatus = loadResult.libraryStatus;
    else if (!loadResult.playlistsStatus.is_ok()) lastLoadStatus = loadResult.playlistsStatus;
    else if (!loadResult.historyStatus.is_ok()) lastLoadStatus = loadResult.historyStatus;
    else if (!loadResult.watchedFoldersStatus.is_ok()) lastLoadStatus = loadResult.watchedFoldersStatus;
    else lastLoadStatus = StorageStatus::Ok(dataDir);
    return lastLoadStatus;
}

std::shared_ptr<const MediaLibrarySaveSnapshot> MediaLibrary::CaptureSaveSnapshot(
    const std::shared_ptr<const MediaLibrarySaveSnapshot>& previous) const {
    using namespace LibrarySaveComparison;
    if (previous && previous->rootDir != portableRoot) return CaptureSaveSnapshot({});
    std::shared_ptr<const MediaLibrarySaveSnapshot::MediaRecords> media = previous ? previous->mediaItems : nullptr;
    std::shared_ptr<MediaLibrarySaveSnapshot::MediaRecords> changedMedia;
    std::unordered_map<std::string_view, size_t> previousById;
    bool builtIdLookup = false;
    const bool sizeChanged = !media || media->size() != mediaItems.size();
    for (size_t i = 0; i < mediaItems.size(); ++i) {
        const auto& item = mediaItems[i];
        std::shared_ptr<const MediaItem> old;
        if (media && i < media->size()) old = (*media)[i];
        const bool samePosition = old && SameMedia(item, *old);
        if (samePosition && !sizeChanged && !changedMedia) continue;
        if (!changedMedia) {
            changedMedia = std::make_shared<MediaLibrarySaveSnapshot::MediaRecords>();
            changedMedia->reserve(mediaItems.size());
            if (media) changedMedia->insert(changedMedia->end(), media->begin(), media->begin() + i);
        }
        // Only membership/reordering needs the temporary ID lookup. Ordinary
        // progress updates share every unaffected record without normalization.
        if (!samePosition && media && (!old || item.id != old->id) && !item.id.empty()) {
            if (!builtIdLookup) {
                previousById.reserve(media->size());
                for (size_t j = 0; j < media->size(); ++j) previousById.emplace((*media)[j]->id, j);
                builtIdLookup = true;
            }
            const auto it = previousById.find(item.id);
            if (it != previousById.end()) old = (*media)[it->second];
        }
        changedMedia->push_back(samePosition || (old && SameMedia(item, *old)) ? old : std::make_shared<const MediaItem>(item));
    }
    if (!changedMedia && sizeChanged) {
        changedMedia = std::make_shared<MediaLibrarySaveSnapshot::MediaRecords>();
        if (media) changedMedia->assign(media->begin(), media->begin() + mediaItems.size());
    }
    if (changedMedia) media = changedMedia;
    const auto capturedPlaylists = CaptureSequence(playlists, previous ? previous->playlists : nullptr, SamePlaylist);
    const auto capturedHistory = CaptureSequence(history, previous ? previous->history : nullptr, SameHistory);
    const auto capturedFolders = CaptureSequence(watchedFolders, previous ? previous->watchedFolders : nullptr,
        [](const auto& a, const auto& b) { return a == b; });
    const auto sameStatus = [](const StorageStatus& a, const StorageStatus& b) {
        return std::tie(a.success, a.code, a.win32Error, a.path, a.message) ==
               std::tie(b.success, b.code, b.win32Error, b.path, b.message);
    };
    if (previous && media == previous->mediaItems && capturedPlaylists == previous->playlists &&
        capturedHistory == previous->history && capturedFolders == previous->watchedFolders &&
        sameStatus(lastLoadResult.libraryStatus, previous->loadResult.libraryStatus) &&
        sameStatus(lastLoadResult.playlistsStatus, previous->loadResult.playlistsStatus) &&
        sameStatus(lastLoadResult.historyStatus, previous->loadResult.historyStatus) &&
        sameStatus(lastLoadResult.watchedFoldersStatus, previous->loadResult.watchedFoldersStatus)) return previous;
    static std::atomic<uint64_t> nextSeriesId{0};
    auto snapshot = std::make_shared<MediaLibrarySaveSnapshot>();
    snapshot->rootDir = portableRoot;
    snapshot->seriesId = previous ? previous->seriesId : nextSeriesId.fetch_add(1) + 1;
    snapshot->libraryGeneration = previous ? previous->libraryGeneration + (media != previous->mediaItems) : 1;
    snapshot->playlistsGeneration = previous ? previous->playlistsGeneration + (capturedPlaylists != previous->playlists) : 1;
    snapshot->historyGeneration = previous ? previous->historyGeneration + (capturedHistory != previous->history) : 1;
    snapshot->watchedFoldersGeneration = previous ? previous->watchedFoldersGeneration + (capturedFolders != previous->watchedFolders) : 1;
    snapshot->mediaItems = std::move(media);
    snapshot->playlists = capturedPlaylists;
    snapshot->history = capturedHistory;
    snapshot->watchedFolders = capturedFolders;
    snapshot->loadResult = lastLoadResult;
    return snapshot;
}

MediaLibrarySaveResult WriteLibrarySaveSnapshot(const MediaLibrarySaveSnapshot& snapshot, MediaLibrarySaveState& state) {
    MediaLibrarySaveResult result;
    if (snapshot.rootDir.empty() || !snapshot.seriesId || !snapshot.mediaItems || !snapshot.playlists ||
        !snapshot.history || !snapshot.watchedFolders) {
        result.success = false;
        result.libraryStatus = result.playlistsStatus = result.historyStatus = result.watchedFoldersStatus =
            StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, snapshot.rootDir,
                                 "Incomplete library save snapshot");
        return result;
    }
    if (state.seriesId != snapshot.seriesId) { state = {}; state.seriesId = snapshot.seriesId; }
    const std::wstring dataDir = snapshot.rootDir + L"\\data";
    bool checkedWritability = false;
    StorageStatus writability;
    auto saveDocument = [&](const wchar_t* file, uint64_t generation, uint64_t& savedGeneration,
                            std::string& lastJson, const StorageStatus& loadStatus,
                            StorageStatus& status, bool& saved, const auto& serialize) {
        const std::wstring path = dataDir + L"\\" + file;
        if (!loadStatus.is_ok()) { status = loadStatus; result.success = false; return; }
        if (generation == savedGeneration && FileExists(path)) { status = StorageStatus::Ok(path); return; }
        if (!checkedWritability) {
            writability = CheckDataDirectoryWritability(snapshot.rootDir);
            checkedWritability = true;
        }
        if (!writability.is_ok()) { status = writability; result.success = false; return; }
        try {
            ++state.serializedStoreCount;
            std::string json = serialize();
            if (json == lastJson && FileExists(path)) {
                status = StorageStatus::Ok(path); savedGeneration = generation; return;
            }
            status = CreateRotatingBackup(path, 3);
            if (!status.is_ok()) { result.success = false; return; }
            status = AtomicWriteTextFile(path, json);
            if (status.is_ok()) { lastJson = std::move(json); savedGeneration = generation; saved = true; }
            else result.success = false;
        } catch (...) {
            status = StorageStatus::Error(StorageErrorCode::SerializationFailed, 0, path, "Failed to serialize library store");
            result.success = false;
        }
    };
    std::vector<MediaLibrarySaveState::RecordSpan> preparedSpans;
    bool preparedMedia = false;
    saveDocument(L"library.json", snapshot.libraryGeneration, state.libraryGeneration,
        state.lastLibraryJson, snapshot.loadResult.libraryStatus, result.libraryStatus, result.librarySaved, [&]() {
            CompactJsonArray output(state.lastLibraryJson.size());
            preparedSpans.reserve(snapshot.mediaItems->size());
            const auto& previous = state.lastMediaRecords;
            const bool canReuse = previous && previous->size() == state.lastMediaSpans.size();
            std::unordered_map<const MediaItem*, size_t> previousByRecord;
            bool indexedPrevious = false;
            for (size_t i = 0; i < snapshot.mediaItems->size(); ++i) {
                const auto& record = (*snapshot.mediaItems)[i];
                size_t oldIndex = i;
                bool found = canReuse && i < previous->size() && (*previous)[i] == record;
                if (!found && canReuse) {
                    // A changed record usually stays in place. Only membership
                    // changes/reorders need a temporary identity lookup.
                    const bool displaced = i >= previous->size() || (*previous)[i]->id != record->id;
                    if (displaced) {
                        if (!indexedPrevious) {
                            previousByRecord.reserve(previous->size());
                            for (size_t j = 0; j < previous->size(); ++j) previousByRecord.emplace((*previous)[j].get(), j);
                            indexedPrevious = true;
                        }
                        const auto match = previousByRecord.find(record.get());
                        if (match != previousByRecord.end()) { oldIndex = match->second; found = true; }
                    }
                }
                std::string& text = output.BeginRecord();
                const size_t offset = text.size();
                if (found) {
                    const auto& span = state.lastMediaSpans[oldIndex];
                    // Spans refer only to the last successfully published bytes.
                    text.append(state.lastLibraryJson, span.offset, span.length);
                    ++state.reusedMediaRecordCount;
                } else {
                    AppendMediaRecord(text, *record, snapshot.rootDir);
                    ++state.serializedMediaRecordCount;
                }
                preparedSpans.push_back({offset, text.size() - offset});
            }
            preparedMedia = true;
            return output.Finish();
    });
    // Advancing this metadata before the write succeeds would make a retry
    // pair new record identities with stale bytes. Moves below cannot allocate.
    if (preparedMedia && result.libraryStatus.is_ok() && state.libraryGeneration == snapshot.libraryGeneration) {
        state.lastMediaRecords = snapshot.mediaItems;
        state.lastMediaSpans = std::move(preparedSpans);
    }
    saveDocument(L"playlists.json", snapshot.playlistsGeneration, state.playlistsGeneration,
        state.lastPlaylistsJson, snapshot.loadResult.playlistsStatus, result.playlistsStatus, result.playlistsSaved, [&]() {
            CompactJsonArray p(state.lastPlaylistsJson.size());
            for (const auto& x : *snapshot.playlists) {
                JsonValue j = x.extraFields.is_object() ? x.extraFields : JsonValue::Object();
                j["id"] = x.id;
                j["name"] = x.name;
                j["createdTimestamp"] = x.createdTimestamp;
                j["type"] = x.IsSmart() ? "smart" : "manual";
                j["isSmart"] = x.IsSmart();
                if (x.IsSmart()) {
                    j["smartCriteria"] = x.smartCriteria.ToJson();
                    if (j.is_object()) { j.as_object().erase("itemPaths"); }
                } else {
                    JsonValue a = JsonValue::Array();
                    a.as_array().reserve(x.itemPaths.size());
                    for (const auto& q : x.itemPaths) { a.push_back(JsonValue(Utf16ToUtf8(ToRelocatablePath(q, snapshot.rootDir)))); }
                    j["itemPaths"] = std::move(a);
                    if (j.is_object()) { j.as_object().erase("smartCriteria"); }
                }
                p.Append(j);
            }
            return p.Finish();
    });
    saveDocument(L"history.json", snapshot.historyGeneration, state.historyGeneration,
        state.lastHistoryJson, snapshot.loadResult.historyStatus, result.historyStatus, result.historySaved, [&]() {
            CompactJsonArray h(state.lastHistoryJson.size());
            for (const auto& x : *snapshot.history) {
                JsonValue j = x.extraFields.is_object() ? x.extraFields : JsonValue::Object();
                j["filePath"] = Utf16ToUtf8(ToRelocatablePath(x.filePath, snapshot.rootDir));
                j["title"] = Utf16ToUtf8(x.title);
                j["lastPosSec"] = x.lastPosSec;
                j["durationSec"] = x.durationSec;
                j["watchedTimestamp"] = x.watchedTimestamp;
                h.Append(j);
            }
            return h.Finish();
    });
    saveDocument(L"watched_folders.json", snapshot.watchedFoldersGeneration, state.watchedFoldersGeneration,
        state.lastWatchedFoldersJson, snapshot.loadResult.watchedFoldersStatus, result.watchedFoldersStatus, result.watchedFoldersSaved, [&]() {
            JsonValue wf = JsonValue::Object();
            wf["schemaVersion"] = MediaLibrary::kCurrentWatchedFoldersSchemaVersion;
            JsonValue wfArray = JsonValue::Array();
            wfArray.as_array().reserve(snapshot.watchedFolders->size());
            for (const auto& folder : *snapshot.watchedFolders) { wfArray.push_back(JsonValue(Utf16ToUtf8(ToRelocatablePath(folder, snapshot.rootDir)))); }
            wf["folders"] = std::move(wfArray);
            return wf.stringify(false);
    });
    return result;
}

MediaLibrarySaveResult MediaLibrary::Save() {
    if (saveRequestHandler && saveRequestOwner == this && saveRequestThread == std::this_thread::get_id()) {
        return saveRequestHandler();
    }
    try {
        lastSaveSnapshot = CaptureSaveSnapshot(lastSaveSnapshot);
        lastSaveResult = WriteLibrarySaveSnapshot(*lastSaveSnapshot, saveState);
    } catch (...) {
        lastSaveResult = {};
        lastSaveResult.success = false;
        lastSaveResult.libraryStatus = lastSaveResult.playlistsStatus = lastSaveResult.historyStatus = lastSaveResult.watchedFoldersStatus =
            StorageStatus::Error(StorageErrorCode::SerializationFailed, 0, portableRoot, "Failed to capture library data for saving");
    }
    return lastSaveResult;
}

MetadataBackfillMergeResult MediaLibrary::MergeMetadataBackfill(const std::vector<MetadataBackfillBaseline>& baseline,
    const std::vector<MediaItem>& updates) {
    MetadataBackfillMergeResult result;
    bool catalogChanged = false, playbackChanged = false;
    for (size_t i = 0; i < updates.size(); ++i) {
        const auto& update = updates[i];
        if (i >= baseline.size() || update.id != baseline[i].id ||
            NormalizePathKey(update.filePath) != NormalizePathKey(baseline[i].path)) {
            ++result.discardedCount; continue;
        }
        auto* live = update.id.empty() ? FindByPath(update.filePath) : FindById(update.id);
        if (!live || !baseline[i].Matches(*live)) { ++result.discardedCount; continue; }
        const bool metadataChanged = !MetadataBackfillBaseline::SameDuration(live->durationSec, update.durationSec) ||
            live->videoWidth != update.videoWidth || live->videoHeight != update.videoHeight ||
            live->fileSizeBytes != update.fileSizeBytes || live->fileIndex != update.fileIndex ||
            live->volumeSerialNumber != update.volumeSerialNumber;
        const bool retryChanged = live->metadataProbeFailed != update.metadataProbeFailed ||
            live->probeAttemptCount != update.probeAttemptCount || live->lastProbeAttemptTimestamp != update.lastProbeAttemptTimestamp;
        if (!metadataChanged && !retryChanged) continue;
        live->durationSec = update.durationSec;
        live->videoWidth = update.videoWidth; live->videoHeight = update.videoHeight;
        live->fileSizeBytes = update.fileSizeBytes;
        live->fileIndex = update.fileIndex; live->volumeSerialNumber = update.volumeSerialNumber;
        live->metadataProbeFailed = update.metadataProbeFailed;
        live->probeAttemptCount = update.probeAttemptCount; live->lastProbeAttemptTimestamp = update.lastProbeAttemptTimestamp;
        if (std::isfinite(live->durationSec) && live->durationSec > 0 && live->lastPosSec > live->durationSec) {
            live->lastPosSec = live->durationSec;
            playbackChanged = true;
        }
        catalogChanged |= metadataChanged;
        ++result.changedCount;
    }
    if (result.changedCount) ++libraryVersion;
    if (catalogChanged) ++revisions.catalog;
    if (playbackChanged) ++revisions.playback;
    result.viewChanged = catalogChanged || playbackChanged;
    if (result.viewChanged) InvalidatePlaylistStatsCache();
    // IDs, paths and vector positions did not change. Lookup maps and search
    // text remain valid; retry bookkeeping does not invalidate UI projections.
    return result;
}

bool MediaLibrary::NeedsMetadataBackfill(const MediaItem& item) {
    return ClassifyPlaybackTarget(item.filePath) == PlaybackTargetKind::LocalFile &&
        (!std::isfinite(item.durationSec) || item.durationSec <= 0 || item.metadataProbeFailed);
}

bool MediaLibrary::CanAttemptMetadataBackfill(const MediaItem& item, const MetadataBackfillOptions& options, int64_t now) {
    if (!NeedsMetadataBackfill(item)) return false;
    if (options.forceRetry) return true;
    if (options.maxAttempts > 0 && item.probeAttemptCount >= options.maxAttempts) return false;
    if (item.lastProbeAttemptTimestamp <= 0) return true;
    const unsigned shift = (std::min)(item.probeAttemptCount > 0 ? item.probeAttemptCount - 1 : 0u, 30u);
    const int64_t maximum = (std::max)(int64_t{0}, options.maxBackoffSec);
    const int64_t base = (std::max)(int64_t{0}, options.baseBackoffSec);
    const int64_t multiplier = int64_t{1} << shift;
    const int64_t backoff = base > maximum / multiplier ? maximum : (std::min)(base * multiplier, maximum);
    return now >= item.lastProbeAttemptTimestamp && now - item.lastProbeAttemptTimestamp >= backoff;
}

MetadataBackfillResult MediaLibrary::BackfillMissingMetadata(const MetadataBackfillOptions& options,
                                                             const MetadataBackfillProgressCallback& progress, bool saveChanges) {
    MetadataBackfillResult result;
    const int64_t now = options.nowTimestamp > 0 ? options.nowTimestamp : GetCurrentTimestampSec();
    const auto probeFn = options.probeFunction ? options.probeFunction : ProbeMediaMetadata;
    std::vector<size_t> eligibleIndices;
    eligibleIndices.reserve(mediaItems.size());
    for (size_t i = 0; i < mediaItems.size(); ++i) {
        const auto& item = mediaItems[i];
        if (!NeedsMetadataBackfill(item)) continue;
        result.totalEligible++;
        if (CanAttemptMetadataBackfill(item, options, now)) eligibleIndices.push_back(i);
        else result.skippedBackoffCount++;
    }
    if (options.maxItems > 0 && eligibleIndices.size() > options.maxItems) { eligibleIndices.resize(options.maxItems); }
    if (progress && !progress(0, eligibleIndices.size(), L"")) { result.outcome = LongOperationOutcome::Canceled; return result; }
    bool anyChanged = false;
    for (size_t idx : eligibleIndices) {
        MediaItem& item = mediaItems[idx];
        if (progress && !progress(result.processedCount, eligibleIndices.size(), item.filePath)) {
            result.outcome = LongOperationOutcome::Canceled;
            break;
        }
        result.processedCount++;
        const bool fileExistsOnDisk = FileExists(item.filePath);
        double duration = 0.0;
        uint32_t width = 0, height = 0;
        const bool probed = fileExistsOnDisk && probeFn(item.filePath, duration, width, height);
        if (probed) { item.videoWidth = width; item.videoHeight = height; }
        if (probed && std::isfinite(duration) && duration > 0) {
            item.durationSec = std::max(0.0, duration);
            item.videoWidth = width;
            item.videoHeight = height;
            item.fileSizeBytes = GetFileSizeInBytes(item.filePath);
            const WindowsFileIdentity fid = GetWindowsFileIdentity(item.filePath);
            if (fid.valid) { item.fileIndex = fid.fileIndex; item.volumeSerialNumber = fid.volumeSerialNumber; }
            if (item.durationSec > 0.0) { item.lastPosSec = std::min(item.lastPosSec, item.durationSec); }
            item.metadataProbeFailed = false;
            item.probeAttemptCount = 0;
            item.lastProbeAttemptTimestamp = 0;
            result.completedCount++;
            anyChanged = true;
        } else {
            item.metadataProbeFailed = true;
            item.probeAttemptCount++;
            item.lastProbeAttemptTimestamp = now;
            result.failedCount++;
            anyChanged = true;
        }
    }
    if (result.outcome != LongOperationOutcome::Canceled) { result.outcome = LongOperationOutcome::Completed; }
    if (anyChanged) {
        libraryVersion++; ++revisions.catalog;
        InvalidatePlaylistStatsCache();
        if (saveChanges) {
            MediaLibrarySaveResult saveRes = Save();
            if (!saveRes.is_ok()) { result.status = saveRes.libraryStatus; }
        }
    }
    return result;
}

// ============================================================================
// Smart Playlist Rule & Criteria Implementation
// ============================================================================

static std::string ToLowerStr(const std::string& str) { std::string s = str; for (auto& c : s) c = (char)::tolower(static_cast<unsigned char>(c)); return s; }

static std::wstring ToLowerWStr(const std::wstring& str) { std::wstring s = str; for (auto& c : s) c = (wchar_t)::towlower(c); return s; }

static std::string TrimStr(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n' || s[start] == '"')) ++start;
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n' || s[end - 1] == '"')) --end;
    return s.substr(start, end - start);
}

static bool StringOpMatches(const std::wstring& target, SmartRuleOperator op, const std::wstring& pattern) {
    if (op == SmartRuleOperator::Contains) return target.find(pattern) != std::wstring::npos;
    if (op == SmartRuleOperator::DoesNotContain) return target.find(pattern) == std::wstring::npos;
    if (op == SmartRuleOperator::Equals) return target == pattern;
    if (op == SmartRuleOperator::NotEquals) return target != pattern;
    if (op == SmartRuleOperator::StartsWith) return target.rfind(pattern, 0) == 0;
    if (op == SmartRuleOperator::EndsWith)
        return target.size() >= pattern.size() && target.compare(target.size() - pattern.size(), pattern.size(), pattern) == 0;
    return target.find(pattern) != std::wstring::npos;
}

template <typename T> static bool NumericOpMatches(T target, SmartRuleOperator op, T expected) {
    if (op == SmartRuleOperator::Equals) return target == expected;
    if (op == SmartRuleOperator::NotEquals) return target != expected;
    if (op == SmartRuleOperator::GreaterThan) return target > expected;
    if (op == SmartRuleOperator::LessThan) return target < expected;
    if (op == SmartRuleOperator::GreaterThanOrEqual) return target >= expected;
    if (op == SmartRuleOperator::LessThanOrEqual) return target <= expected;
    return target == expected;
}

const std::wstring& SmartPlaylistRule::GetLowerPattern() const {
    if (lowerPatternCache.empty() || lowerPatternSource != value) {
        lowerPatternSource = value;
        lowerPatternCache = ToLowerWStr(Utf8ToUtf16(value));
    }
    return lowerPatternCache;
}

bool SmartPlaylistRule::Validate(std::string* error) const {
    bool valid = static_cast<int>(field) >= 0 && field <= SmartRuleField::VideoHeight &&
        static_cast<int>(op) >= 0 && op <= SmartRuleOperator::EndsWith;
    if (valid && field >= SmartRuleField::Duration) {
        if (numericField != static_cast<int>(field) || numericSource != value) {
            numericSource = value;
            numericField = static_cast<int>(field);
            numericValid = SmartRuleParsing::Parse(field, ToLowerStr(TrimStr(value)), numericValue, relativeDate);
        }
        valid = numericValid;
    }
    if (!valid && error) *error = "Invalid value or operator in smart playlist rule: " + ToString();
    return valid;
}

bool SmartPlaylistRule::Matches(const MediaItem& item, bool existsOnDisk) const {
    if (!Validate()) return false;
    switch (field) {
        case SmartRuleField::Title: {
            std::wstring target = ToLowerWStr(item.GetDisplayTitle());
            return StringOpMatches(target, op, GetLowerPattern());
        }
        case SmartRuleField::Path: {
            std::wstring target = ToLowerWStr(item.filePath);
            return StringOpMatches(target, op, GetLowerPattern());
        }
        case SmartRuleField::Status: {
            std::string pattern = ToLowerStr(TrimStr(value));
            std::string normPattern = pattern;
            if (pattern == "unwatched" || pattern == "new") normPattern = "new";
            else if (pattern == "in_progress" || pattern == "inprogress" || pattern == "watching") normPattern = "in_progress";
            else if (pattern == "done" || pattern == "watched" || pattern == "completed") normPattern = "done";
            if (pattern == "missing") {
                bool isMissing = !existsOnDisk;
                if (op == SmartRuleOperator::Equals || op == SmartRuleOperator::Contains) return isMissing;
                if (op == SmartRuleOperator::NotEquals || op == SmartRuleOperator::DoesNotContain) return !isMissing;
            }
            if (op == SmartRuleOperator::Equals || op == SmartRuleOperator::Contains) { return item.status == normPattern; }
            if (op == SmartRuleOperator::NotEquals || op == SmartRuleOperator::DoesNotContain) { return item.status != normPattern; }
            return item.status == normPattern;
        }
        case SmartRuleField::Duration: return NumericOpMatches(item.durationSec, op, static_cast<double>(numericValue));
        case SmartRuleField::Progress: {
            double exp = static_cast<double>(numericValue);
            return NumericOpMatches(item.GetProgressRatio(), op, exp);
        }
        case SmartRuleField::FileSize: return NumericOpMatches(item.fileSizeBytes, op, static_cast<uint64_t>(numericValue));
        case SmartRuleField::DateAdded:
        case SmartRuleField::LastWatched: {
            const int64_t valueSeconds = static_cast<int64_t>(numericValue);
            const int64_t exp = relativeDate ? GetCurrentTimestampSec() - valueSeconds : valueSeconds;
            return NumericOpMatches(field == SmartRuleField::DateAdded ? item.addedTimestamp : item.lastWatchedTimestamp, op, exp);
        }
        case SmartRuleField::VideoHeight: {
            uint32_t exp = static_cast<uint32_t>(numericValue);
            return NumericOpMatches(item.videoHeight, op, exp);
        }
    }
    return true;
}

static std::string SmartFieldToString(SmartRuleField f) {
    switch (f) {
        case SmartRuleField::Title: return "title";
        case SmartRuleField::Path: return "path";
        case SmartRuleField::Status: return "status";
        case SmartRuleField::Duration: return "duration";
        case SmartRuleField::Progress: return "progress";
        case SmartRuleField::FileSize: return "fileSize";
        case SmartRuleField::DateAdded: return "dateAdded";
        case SmartRuleField::LastWatched: return "lastWatched";
        case SmartRuleField::VideoHeight: return "videoHeight";
    }
    return "title";
}

static SmartRuleField SmartFieldFromString(const std::string& str) {
    std::string s = ToLowerStr(TrimStr(str));
    if (s == "title" || s == "name") return SmartRuleField::Title;
    if (s == "path" || s == "filepath" || s == "folder" || s == "filename" || s == "file" || s == "dir") return SmartRuleField::Path;
    if (s == "status" || s == "state") return SmartRuleField::Status;
    if (s == "duration" || s == "length" || s == "time") return SmartRuleField::Duration;
    if (s == "progress" || s == "percent") return SmartRuleField::Progress;
    if (s == "filesize" || s == "size" || s == "bytes") return SmartRuleField::FileSize;
    if (s == "dateadded" || s == "added" || s == "date") return SmartRuleField::DateAdded;
    if (s == "lastwatched" || s == "watched" || s == "lastplayed" || s == "played") return SmartRuleField::LastWatched;
    if (s == "videoheight" || s == "height" || s == "resolution" || s == "res") return SmartRuleField::VideoHeight;
    return static_cast<SmartRuleField>(-1);
}

static std::string SmartOpToString(SmartRuleOperator op) {
    switch (op) {
        case SmartRuleOperator::Contains: return "contains";
        case SmartRuleOperator::DoesNotContain: return "doesNotContain";
        case SmartRuleOperator::Equals: return "equals";
        case SmartRuleOperator::NotEquals: return "notEquals";
        case SmartRuleOperator::GreaterThan: return "greaterThan";
        case SmartRuleOperator::LessThan: return "lessThan";
        case SmartRuleOperator::GreaterThanOrEqual: return "greaterThanOrEqual";
        case SmartRuleOperator::LessThanOrEqual: return "lessThanOrEqual";
        case SmartRuleOperator::StartsWith: return "startsWith";
        case SmartRuleOperator::EndsWith: return "endsWith";
    }
    return "contains";
}

static SmartRuleOperator SmartOpFromString(const std::string& str) {
    std::string s = ToLowerStr(TrimStr(str));
    if (s == "contains" || s == "has" || s == "like" || s == ":") return SmartRuleOperator::Contains;
    if (s == "doesnotcontain" || s == "notcontains" || s == "!contains" || s == "!has") return SmartRuleOperator::DoesNotContain;
    if (s == "equals" || s == "equal" || s == "eq" || s == "is" || s == "==" || s == "=") return SmartRuleOperator::Equals;
    if (s == "notequals" || s == "notequal" || s == "ne" || s == "isnot" || s == "!=" || s == "<>") return SmartRuleOperator::NotEquals;
    if (s == "greaterthan" || s == "gt" || s == ">") return SmartRuleOperator::GreaterThan;
    if (s == "lessthan" || s == "lt" || s == "<") return SmartRuleOperator::LessThan;
    if (s == "greaterthanorequal" || s == "gte" || s == "ge" || s == ">=") return SmartRuleOperator::GreaterThanOrEqual;
    if (s == "lessthanorequal" || s == "lte" || s == "le" || s == "<=") return SmartRuleOperator::LessThanOrEqual;
    if (s == "startswith" || s == "starts_with") return SmartRuleOperator::StartsWith;
    if (s == "endswith" || s == "ends_with") return SmartRuleOperator::EndsWith;
    return static_cast<SmartRuleOperator>(-1);
}

JsonValue SmartPlaylistRule::ToJson() const {
    JsonValue obj = JsonValue::Object();
    obj["field"] = SmartFieldToString(field);
    obj["op"] = SmartOpToString(op);
    obj["value"] = value;
    return obj;
}

SmartPlaylistRule SmartPlaylistRule::FromJson(const JsonValue& json) {
    SmartPlaylistRule rule;
    if (!json.is_object() || !json.get("field").is_string() || !json.get("op").is_string() || !json.get("value").is_string()) {
        rule.field = static_cast<SmartRuleField>(-1); return rule;
    }
    if (json.is_object()) {
        rule.field = SmartFieldFromString(json.get("field").as_string("title"));
        rule.op = SmartOpFromString(json.get("op").as_string("contains"));
        rule.value = json.get("value").as_string();
    }
    return rule;
}

std::string SmartPlaylistRule::ToString() const {
    std::string opSymbol = "contains";
    if (op == SmartRuleOperator::Equals) opSymbol = "=";
    else if (op == SmartRuleOperator::NotEquals) opSymbol = "!=";
    else if (op == SmartRuleOperator::GreaterThan) opSymbol = ">";
    else if (op == SmartRuleOperator::LessThan) opSymbol = "<";
    else if (op == SmartRuleOperator::GreaterThanOrEqual) opSymbol = ">=";
    else if (op == SmartRuleOperator::LessThanOrEqual) opSymbol = "<=";
    else if (op == SmartRuleOperator::DoesNotContain) opSymbol = "!contains";
    else if (op == SmartRuleOperator::StartsWith) opSymbol = "startswith";
    else if (op == SmartRuleOperator::EndsWith) opSymbol = "endswith";
    return SmartFieldToString(field) + " " + opSymbol + " " + value;
}

SmartPlaylistRule SmartPlaylistRule::FromString(const std::string& str) {
    SmartPlaylistRule rule;
    std::string s = TrimStr(str);
    if (s.empty()) return rule;
    const std::string lower = ToLowerStr(s);
    static const std::vector<std::pair<std::string, SmartRuleOperator>> ops = {
        { ">=", SmartRuleOperator::GreaterThanOrEqual }, { "<=", SmartRuleOperator::LessThanOrEqual },
        { "!=", SmartRuleOperator::NotEquals }, { "==", SmartRuleOperator::Equals }, { "!contains", SmartRuleOperator::DoesNotContain },
        { "doesnotcontain", SmartRuleOperator::DoesNotContain }, { "startswith", SmartRuleOperator::StartsWith },
        { "endswith", SmartRuleOperator::EndsWith }, { "contains", SmartRuleOperator::Contains }, { "isnot", SmartRuleOperator::NotEquals },
        { "is", SmartRuleOperator::Equals }, { ">", SmartRuleOperator::GreaterThan }, { "<", SmartRuleOperator::LessThan },
        { "=", SmartRuleOperator::Equals }, { ":", SmartRuleOperator::Contains }
    };
    for (const auto& opPair : ops) {
        size_t pos = lower.find(opPair.first);
        if (pos != std::string::npos && pos > 0) {
            std::string fieldPart = TrimStr(s.substr(0, pos));
            std::string valPart = TrimStr(s.substr(pos + opPair.first.size()));
            rule.field = SmartFieldFromString(fieldPart);
            // A word operator inside the value (e.g. "is" in "missing")
            // cannot replace the operator following the recognized field.
            if (static_cast<int>(rule.field) < 0) continue;
            rule.op = opPair.second;
            rule.value = valPart;
            return rule;
        }
    }
    rule.field = SmartRuleField::Title;
    rule.op = SmartRuleOperator::Contains;
    rule.value = s;
    return rule;
}

bool SmartPlaylistCriteria::Validate(std::string* error) const {
    if (!validationError.empty() || static_cast<int>(matchMode) < 0 || matchMode > SmartMatchMode::MatchAny ||
        sortField < 0 || sortField > static_cast<int>(LibrarySortField::Manual) ||
        sortDirection < 0 || sortDirection > 1 || limit > JsonValue::kMaxJsonContainerElements) {
        if (error) *error = validationError.empty() ? "Invalid smart playlist sort or limit" : validationError;
        return false;
    }
    for (const auto& rule : rules) if (!rule.Validate(error)) return false;
    return true;
}

bool SmartPlaylistCriteria::RequiresAvailability() const {
    for (const auto& rule : rules)
        if (rule.field == SmartRuleField::Status && ToLowerStr(TrimStr(rule.value)) == "missing") return true;
    return false;
}

bool SmartPlaylistCriteria::Matches(const MediaItem& item, bool existsOnDisk) const {
    if (!validationError.empty()) return false;
    if (rules.empty()) return true;
    if (matchMode == SmartMatchMode::MatchAll) {
        for (const auto& rule : rules) { if (!rule.Matches(item, existsOnDisk)) return false; }
        return true;
    } else {
        for (const auto& rule : rules) { if (rule.Matches(item, existsOnDisk)) return true; }
        return false;
    }
}

JsonValue SmartPlaylistCriteria::ToJson() const {
    if (hasInvalidSource && !Validate()) return invalidSource;
    JsonValue obj = JsonValue::Object();
    obj["matchMode"] = (matchMode == SmartMatchMode::MatchAll) ? "all" : "any";
    obj["sortField"] = sortField;
    obj["sortDirection"] = sortDirection;
    obj["limit"] = limit;
    JsonValue rulesArr = JsonValue::Array();
    for (const auto& r : rules) { rulesArr.push_back(r.ToJson()); }
    obj["rules"] = std::move(rulesArr);
    return obj;
}

SmartPlaylistCriteria SmartPlaylistCriteria::FromJson(const JsonValue& json) {
    SmartPlaylistCriteria crit;
    if (!json.is_object()) {
        crit.validationError = "Smart playlist criteria must be an object";
        crit.invalidSource = json; crit.hasInvalidSource = true; return crit;
    }
    std::string mode = json.get("matchMode").as_string("all");
    if ((json.has_key("matchMode") && !json.get("matchMode").is_string()) ||
        (mode != "all" && mode != "and" && mode != "any" && mode != "or")) crit.validationError = "Invalid match mode";
    crit.matchMode = (mode == "any" || mode == "or") ? SmartMatchMode::MatchAny : SmartMatchMode::MatchAll;
    const auto boundedInteger = [&](const char* name, size_t maximum) -> size_t {
        if (!json.has_key(name)) return 0;
        const auto& value = json.get(name); const double number = value.as_number(-1);
        if (!value.is_number() || !std::isfinite(number) || number < 0 || number > maximum || std::floor(number) != number) {
            crit.validationError = std::string("Invalid smart playlist ") + name; return 0;
        }
        return static_cast<size_t>(number);
    };
    crit.sortField = static_cast<int>(boundedInteger("sortField", static_cast<size_t>(LibrarySortField::Manual)));
    crit.sortDirection = static_cast<int>(boundedInteger("sortDirection", 1));
    crit.limit = boundedInteger("limit", JsonValue::kMaxJsonContainerElements);
    if (json.has_key("rules") && !json.get("rules").is_array()) crit.validationError = "Smart playlist rules must be an array";
    if (json.has_key("rules") && json.get("rules").is_array()) {
        const auto& arr = json.get("rules").as_array();
        crit.rules.reserve(arr.size());
        for (const auto& rJson : arr) { crit.rules.push_back(SmartPlaylistRule::FromJson(rJson)); }
    }
    std::string validation;
    if (!crit.Validate(&validation)) {
        crit.validationError = validation;
        crit.invalidSource = json; crit.hasInvalidSource = true;
    }
    return crit;
}

std::string SmartPlaylistCriteria::ToRuleString() const {
    std::string res = "";
    if (matchMode == SmartMatchMode::MatchAny) res += "match:any; ";
    if (sortField != 0) res += "sort:" + std::to_string(sortField) + "; ";
    if (sortDirection != 0) res += "desc; ";
    if (limit > 0) res += "limit:" + std::to_string(limit) + "; ";
    for (size_t i = 0; i < rules.size(); ++i) { if (i > 0) res += "; "; res += rules[i].ToString(); }
    return res;
}

SmartPlaylistCriteria SmartPlaylistCriteria::FromRuleString(const std::string& str) {
    SmartPlaylistCriteria crit;
    std::string s = TrimStr(str);
    if (s.empty()) return crit;
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ';')) {
        std::string part = TrimStr(token);
        if (part.empty()) continue;
        std::string lowerPart = ToLowerStr(part);
        if (lowerPart == "match:any" || lowerPart == "mode:any" || lowerPart == "any" || lowerPart == "or") {
            crit.matchMode = SmartMatchMode::MatchAny;
        } else if (lowerPart == "match:all" || lowerPart == "mode:all" || lowerPart == "all" || lowerPart == "and") {
            crit.matchMode = SmartMatchMode::MatchAll;
        } else if (lowerPart.rfind("sort:", 0) == 0) {
            std::string sVal = lowerPart.substr(5);
            if (sVal == "title") crit.sortField = 0;
            else if (sVal == "date" || sVal == "dateadded" || sVal == "added") crit.sortField = 1;
            else if (sVal == "played" || sVal == "lastplayed" || sVal == "watched") crit.sortField = 2;
            else if (sVal == "duration" || sVal == "time") crit.sortField = 3;
            else if (sVal == "progress") crit.sortField = 4;
            else {
                try { size_t consumed = 0; crit.sortField = std::stoi(sVal, &consumed); if (consumed != sVal.size()) crit.validationError = "Invalid sort field"; }
                catch (...) { crit.validationError = "Invalid sort field"; }
            }
        } else if (lowerPart == "asc" || lowerPart == "ascending" || lowerPart == "order:asc") {
            crit.sortDirection = 0;
        } else if (lowerPart == "desc" || lowerPart == "descending" || lowerPart == "order:desc") {
            crit.sortDirection = 1;
        } else if (lowerPart.rfind("limit:", 0) == 0 || lowerPart.rfind("max:", 0) == 0) {
            size_t colon = lowerPart.find(':');
            try {
                const std::string number = TrimStr(lowerPart.substr(colon + 1));
                size_t consumed = 0;
                const auto limit = std::stoll(number, &consumed);
                if (limit < 0 || consumed != number.size()) crit.validationError = "Invalid result limit";
                else crit.limit = static_cast<size_t>(limit);
            } catch (...) { crit.validationError = "Invalid result limit"; }
        } else { crit.rules.push_back(SmartPlaylistRule::FromString(part)); }
    }
    return crit;
}

} // namespace Aperture
