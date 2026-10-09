#pragma once

#include "Common.h"
#include "Json.h"
#include <bcrypt.h>
#include <string_view>
#pragma comment(lib, "bcrypt.lib")

namespace Aperture::LibraryIncrementalStore {

inline constexpr int kFormatVersion = 1;
inline constexpr size_t kMaxBytes = 1024 * 1024;
inline constexpr size_t kMaxRecords = 512;
inline constexpr std::string_view kHeader = "ApertureLibraryChanges:1\n";

// Checkpoint hashing runs only at full publication/load; routine saves hash
// the bounded changes file. A failed crypto operation never yields a digest.
inline std::string Digest(std::string_view bytes) {
    if (bytes.size() > kMaxPortableStoreBytes || !SystemDllPresent(L"bcrypt.dll")) return {};
    struct Handles {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        ~Handles() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } handles;
    if (BCryptOpenAlgorithmProvider(&handles.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    // Windows 7+ can allocate the hash object itself; the application targets Windows 10+.
    if (BCryptCreateHash(handles.algorithm, &handles.hash, nullptr, 0, nullptr, 0, 0) < 0) return {};
    if (!bytes.empty() && BCryptHashData(handles.hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
        static_cast<ULONG>(bytes.size()), 0) < 0) return {};
    unsigned char digest[32]{};
    if (BCryptFinishHash(handles.hash, digest, sizeof(digest), 0) < 0) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result; result.reserve(64);
    for (unsigned char value : digest) { result.push_back(hex[value >> 4]); result.push_back(hex[value & 15]); }
    return result;
}

inline bool IsDigest(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

inline std::string Encode(const std::string& payload) {
    const auto hash = Digest(payload);
    return hash.empty() ? std::string() : std::string(kHeader) + hash + "\n" + payload;
}

inline DWORD Decode(const std::string& text, JsonValue& payload) {
    if (text.size() > kMaxBytes) return ERROR_FILE_TOO_LARGE;
    if (text.compare(0, kHeader.size(), kHeader) != 0) {
        constexpr std::string_view prefix = "ApertureLibraryChanges:";
        // Do not recover over a well-formed future format header.
        const auto end = text.find('\n');
        if (text.compare(0, prefix.size(), prefix) == 0 && end != std::string::npos) {
            const auto version = text.substr(prefix.size(), end - prefix.size());
            if (!version.empty() && std::all_of(version.begin(), version.end(), [](char c) { return c >= '0' && c <= '9'; }))
                return ERROR_REVISION_MISMATCH;
        }
        return ERROR_INVALID_DATA;
    }
    const size_t body = kHeader.size() + 65;
    if (text.size() <= body || text[body - 1] != '\n') return ERROR_INVALID_DATA;
    const auto expected = text.substr(kHeader.size(), 64);
    const std::string_view content(text.data() + body, text.size() - body);
    if (!IsDigest(expected) || Digest(content) != expected || !JsonValue::parse(std::string(content), payload))
        return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}

inline bool UnsignedSize(const JsonValue& value, size_t maximum, size_t& out) {
    const auto number = value.as_number(-1);
    if (!value.is_number() || !std::isfinite(number) || number < 0 || number > maximum || std::floor(number) != number) return false;
    out = static_cast<size_t>(number); return true;
}

enum class Match { Invalid, DifferentCheckpoint, Applicable };
inline Match Validate(const JsonValue& payload, const std::string& baseDigest, size_t baseBytes, size_t baseCount) {
    if (!payload.is_object() || !IsDigest(payload.get("baseSha256").as_string()) || !payload.get("records").is_array()) return Match::Invalid;
    size_t savedBytes = 0, count = 0;
    if (!UnsignedSize(payload.get("baseBytes"), kMaxPortableStoreBytes, savedBytes) ||
        !UnsignedSize(payload.get("itemCount"), JsonValue::kMaxJsonContainerElements, count)) return Match::Invalid;
    const auto& records = payload.get("records").as_array();
    if (records.size() > kMaxRecords) return Match::Invalid;
    size_t previous = 0;
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& record = records[i];
        size_t index = 0;
        if (!record.is_array() || record.as_array().size() != 2 || !record.as_array()[1].is_object() ||
            !UnsignedSize(record.as_array()[0], count, index) || index >= count || (i && index <= previous)) return Match::Invalid;
        previous = index;
    }
    if (savedBytes != baseBytes || payload.get("baseSha256").as_string() != baseDigest) return Match::DifferentCheckpoint;
    // Appends must fill every new slot; count alone cannot cause a large allocation.
    size_t next = baseCount;
    for (const auto& record : records) {
        const size_t index = static_cast<size_t>(record.as_array()[0].as_number());
        if (index >= baseCount && index != next++) return Match::Invalid;
    }
    if (count > baseCount && next != count) return Match::Invalid;
    return Match::Applicable;
}

inline void Apply(JsonValue& base, JsonValue& payload) {
    auto& items = base.is_array() ? base.as_array() : base["items"].as_array();
    items.resize(static_cast<size_t>(payload.get("itemCount").as_number()));
    for (auto& record : payload["records"].as_array())
        items[static_cast<size_t>(record.as_array()[0].as_number())] = std::move(record.as_array()[1]);
}

inline StorageStatus Load(const std::wstring& path, const std::string& checkpoint, JsonValue& root, bool& recovered) {
    DWORD pathError = ERROR_SUCCESS;
    const auto availability = QueryPathAvailability(path, &pathError);
    if (availability == PathAvailability::Unavailable)
        return StorageStatus::Error(StorageErrorCode::FileReadFailed, pathError, path, "Library changes are unavailable");
    if (availability == PathAvailability::Missing && !recovered) return StorageStatus::Ok(path);
    const auto* items = root.is_array() ? &root : (root.get("items").is_array() ? &root.get("items") : nullptr);
    if (checkpoint.empty() || !items)
        return StorageStatus::Error(StorageErrorCode::SerializationFailed, ERROR_INVALID_DATA, path, "Library changes have no checkpoint");
    const auto baseDigest = Digest(checkpoint);
    if (baseDigest.empty()) return StorageStatus::Error(StorageErrorCode::FileReadFailed, ERROR_INVALID_DATA, path, "Cannot verify library checkpoint");
    auto inspect = [&](const std::string& content, JsonValue& payload) {
        if (checkpoint.size() + content.size() > kMaxPortableStoreBytes || Decode(content, payload) != ERROR_SUCCESS) return Match::Invalid;
        return Validate(payload, baseDigest, checkpoint.size(), items->as_array().size());
    };
    std::string text;
    JsonValue payload;
    Match current = Match::Invalid;
    if (availability == PathAvailability::Present) {
        const auto readStatus = ReadTextFile(path, text, kMaxBytes);
        if (!readStatus.is_ok()) return readStatus;
        const auto decoded = Decode(text, payload);
        if (decoded == ERROR_REVISION_MISMATCH)
            return StorageStatus::Error(StorageErrorCode::SerializationFailed, ERROR_REVISION_MISMATCH, path, "Library changes require a newer application version");
        if (decoded == ERROR_SUCCESS && checkpoint.size() + text.size() <= kMaxPortableStoreBytes)
            current = Validate(payload, baseDigest, checkpoint.size(), items->as_array().size());
        if (current == Match::Applicable) { Apply(root, payload); return StorageStatus::Ok(path); }
        if (current == Match::DifferentCheckpoint && !recovered) return StorageStatus::Ok(path);
    }
    // A corrupt changes file can recover only against its exact checkpoint.
    // If the checkpoint itself was recovered, inspect matching changes backups
    // even after a clean checkpoint removed the active changes file.
    for (size_t i = 1; i <= 3; ++i) {
        const auto slash = path.find_last_of(L"\\/");
        const auto backup = path.substr(0, slash) + L"\\backups\\" + path.substr(slash + 1) + L".bak." + std::to_wstring(i);
        std::string candidate;
        if (ReadTextFile(backup, candidate, kMaxBytes).is_ok() && inspect(candidate, payload) == Match::Applicable) {
            Apply(root, payload); recovered = true; return StorageStatus::Ok(path);
        }
    }
    if (availability == PathAvailability::Missing || current == Match::DifferentCheckpoint) return StorageStatus::Ok(path);
    return StorageStatus::Error(StorageErrorCode::SerializationFailed, ERROR_INVALID_DATA, path, "Invalid library changes and no matching recovery copy");
}

// Hold this read handle during publication: another writer cannot replace the
// checkpoint between the file-fact check and committing its dependent changes.
class CheckpointReadLock {
public:
    CheckpointReadLock(const std::wstring& path, const BY_HANDLE_FILE_INFORMATION& expected) {
        if (!expected.nFileIndexHigh && !expected.nFileIndexLow) return;
        handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        BY_HANDLE_FILE_INFORMATION actual{};
        if (handle_ == INVALID_HANDLE_VALUE) return;
        matches_ = GetFileInformationByHandle(handle_, &actual) &&
            actual.nFileIndexHigh == expected.nFileIndexHigh && actual.nFileIndexLow == expected.nFileIndexLow &&
            actual.dwVolumeSerialNumber == expected.dwVolumeSerialNumber && actual.nFileSizeHigh == expected.nFileSizeHigh &&
            actual.nFileSizeLow == expected.nFileSizeLow && CompareFileTime(&actual.ftLastWriteTime, &expected.ftLastWriteTime) == 0 &&
            !(actual.dwFileAttributes & FILE_ATTRIBUTE_READONLY);
    }
    ~CheckpointReadLock() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    CheckpointReadLock(const CheckpointReadLock&) = delete;
    CheckpointReadLock& operator=(const CheckpointReadLock&) = delete;
    bool Matches() const { return matches_; }
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool matches_ = false;
};

} // namespace Aperture::LibraryIncrementalStore
