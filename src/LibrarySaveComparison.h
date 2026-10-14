#pragma once

#include "MediaLibrary.h"
#include <tuple>

namespace Aperture::LibrarySaveComparison {

template <typename T, typename Equal>
bool SameSequence(const std::vector<T>& a, const std::vector<T>& b, Equal equal) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), equal);
}

template <typename Optional, typename Equal>
bool SameOptional(const Optional& a, const Optional& b, Equal equal) {
    return a.has_value() == b.has_value() && (!a || equal(*a, *b));
}

inline bool SamePreferences(const MediaPlaybackPreferences& a, const MediaPlaybackPreferences& b) {
    auto values = [](const auto& p) {
        return std::tie(p.playbackSpeed, p.aspectRatio, p.audioTrackId, p.audioTrackTitle, p.audioTrackLang,
            p.subtitleTrackId, p.subtitleTrackTitle, p.subtitleTrackLang, p.audioDelayMs, p.subDelayMs,
            p.subtitlesVisible, p.videoRotate, p.brightness, p.contrast, p.saturation, p.gamma, p.hue,
            p.audioBalance, p.audioChannelMap, p.extraFields);
    };
    return values(a) == values(b);
}

inline bool SameBookmark(const MediaBookmark& a, const MediaBookmark& b) {
    return std::tie(a.id, a.timeSec, a.label, a.createdTimestamp, a.extraFields) ==
           std::tie(b.id, b.timeSec, b.label, b.createdTimestamp, b.extraFields);
}

inline bool SameOverrides(const MediaMetadataOverrides& a, const MediaMetadataOverrides& b) {
    return std::tie(a.title, a.year, a.series, a.extraFields) == std::tie(b.title, b.year, b.series, b.extraFields);
}

inline bool SameMedia(const MediaItem& a, const MediaItem& b) {
    auto values = [](const auto& m) {
        return std::tie(m.id, m.filePath, m.title, m.durationSec, m.lastPosSec, m.status, m.fileSizeBytes,
            m.lastWatchedTimestamp, m.addedTimestamp, m.videoWidth, m.videoHeight, m.fileIndex,
            m.volumeSerialNumber, m.metadataFileWriteTime, m.metadataProbeFailed, m.probeAttemptCount, m.lastProbeAttemptTimestamp, m.extraFields);
    };
    return values(a) == values(b) && SameOptional(a.preferences, b.preferences, SamePreferences) &&
           SameSequence(a.bookmarks, b.bookmarks, SameBookmark) && SameOptional(a.overrides, b.overrides, SameOverrides);
}

inline bool SameRule(const SmartPlaylistRule& a, const SmartPlaylistRule& b) {
    return std::tie(a.field, a.op, a.value) == std::tie(b.field, b.op, b.value);
}

inline bool SameCriteria(const SmartPlaylistCriteria& a, const SmartPlaylistCriteria& b) {
    return std::tie(a.matchMode, a.sortField, a.sortDirection, a.limit, a.hasInvalidSource, a.invalidSource) ==
           std::tie(b.matchMode, b.sortField, b.sortDirection, b.limit, b.hasInvalidSource, b.invalidSource) &&
           SameSequence(a.rules, b.rules, SameRule);
}

inline bool SamePlaylist(const Playlist& a, const Playlist& b) {
    return std::tie(a.id, a.name, a.type, a.itemPaths, a.createdTimestamp, a.extraFields) ==
           std::tie(b.id, b.name, b.type, b.itemPaths, b.createdTimestamp, b.extraFields) &&
           SameCriteria(a.smartCriteria, b.smartCriteria);
}

inline bool SameHistory(const HistoryRecord& a, const HistoryRecord& b) {
    return std::tie(a.filePath, a.title, a.lastPosSec, a.durationSec, a.watchedTimestamp, a.extraFields) ==
           std::tie(b.filePath, b.title, b.lastPosSec, b.durationSec, b.watchedTimestamp, b.extraFields);
}

template <typename T, typename Equal>
std::shared_ptr<const std::vector<T>> CaptureSequence(const std::vector<T>& current,
    const std::shared_ptr<const std::vector<T>>& previous, Equal equal) {
    if (previous && SameSequence(current, *previous, equal)) return previous;
    return std::make_shared<const std::vector<T>>(current);
}

} // namespace Aperture::LibrarySaveComparison
