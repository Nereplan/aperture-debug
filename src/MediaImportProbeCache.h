#pragma once

#include "MediaLibrary.h"

namespace Aperture {

struct MediaFileFingerprint {
    uint64_t size = 0, fileIndex = 0, writeTime = 0;
    uint32_t volume = 0;
    bool valid = false;
    bool SameFile(const MediaFileFingerprint& other) const {
        return valid && other.valid && size == other.size && fileIndex == other.fileIndex && volume == other.volume && writeTime == other.writeTime;
    }
};

inline MediaFileFingerprint ReadMediaFileFingerprint(const std::wstring& path) {
    // A writer can delay its last-write timestamp until closing the file.
    // Refuse cache reuse while such a handle is open. This short-lived handle
    // is closed before probing and does not lock the file for the whole probe.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = GetFileInformationByHandle(file, &info);
    CloseHandle(file);
    if (!ok || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return {};
    MediaFileFingerprint fingerprint;
    fingerprint.size = (uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow;
    fingerprint.fileIndex = (uint64_t{info.nFileIndexHigh} << 32) | info.nFileIndexLow;
    fingerprint.writeTime = (uint64_t{info.ftLastWriteTime.dwHighDateTime} << 32) | info.ftLastWriteTime.dwLowDateTime;
    fingerprint.volume = info.dwVolumeSerialNumber;
    fingerprint.valid = fingerprint.fileIndex != 0 && fingerprint.writeTime != 0;
    return fingerprint;
}

class MediaImportProbeCache {
public:
    using Snapshot = std::vector<MetadataBackfillBaseline>;
    static Snapshot Capture(const MediaLibrary& library) {
        Snapshot records; records.reserve(library.mediaItems.size());
        for (const auto& item : library.mediaItems) records.push_back(MetadataBackfillBaseline::Capture(item));
        return records;
    }
    explicit MediaImportProbeCache(Snapshot records) : records_(std::move(records)) {
        byPath_.reserve(records_.size());
        for (size_t i = 0; i < records_.size(); ++i) byPath_[NormalizePathKey(records_[i].path)] = i;
    }
    const MetadataBackfillBaseline* FindByKey(const std::wstring& key) const {
        const auto found = byPath_.find(key);
        return found == byPath_.end() ? nullptr : &records_[found->second];
    }
    const MetadataBackfillBaseline* Find(const std::wstring& path) const { return FindByKey(NormalizePathKey(path)); }
    static bool CanReuse(const MetadataBackfillBaseline& record, const MediaFileFingerprint& fingerprint) {
        return fingerprint.valid && record.fileWriteTime && record.fileWriteTime == fingerprint.writeTime &&
            record.fileIndex == fingerprint.fileIndex && record.volume == fingerprint.volume && record.size == fingerprint.size &&
            std::isfinite(record.durationSec) && record.durationSec > 0 && !record.failed;
    }
private:
    Snapshot records_;
    std::unordered_map<std::wstring, size_t> byPath_;
};

inline bool ProbeMediaWithFingerprint(const std::wstring& path, double& duration, uint32_t& width, uint32_t& height,
    MediaFileFingerprint& observed, const MediaProbeFunction& probe, const MetadataBackfillBaseline* reusable = nullptr,
    bool* reused = nullptr) {
    observed = {};
    if (reused) *reused = false;
    const auto before = ReadMediaFileFingerprint(path);
    if (reusable && MediaImportProbeCache::CanReuse(*reusable, before)) {
        duration = reusable->durationSec; width = reusable->width; height = reusable->height;
        observed = before;
        if (reused) *reused = true;
        return true;
    }
    if (!probe(path, duration, width, height)) return false;
    const auto after = ReadMediaFileFingerprint(path);
    if (before.SameFile(after)) observed = after;
    return true;
}

inline void StampProbedFile(MediaItem& item, const MediaFileFingerprint& observed) {
    item.metadataFileWriteTime = 0;
    if (observed.valid) {
        item.fileSizeBytes = observed.size; item.fileIndex = observed.fileIndex; item.volumeSerialNumber = observed.volume;
        if (std::isfinite(item.durationSec) && item.durationSec > 0 && !item.metadataProbeFailed) item.metadataFileWriteTime = observed.writeTime;
    } else {
        const auto identity = GetWindowsFileIdentity(item.filePath);
        if (identity.valid) { item.fileIndex = identity.fileIndex; item.volumeSerialNumber = identity.volumeSerialNumber; }
    }
}

} // namespace Aperture
