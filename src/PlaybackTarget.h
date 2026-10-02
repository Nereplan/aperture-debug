#pragma once
#include <string>

namespace Aperture {
enum class PlaybackTargetKind { Empty, LocalFile, NetworkStream };
inline PlaybackTargetKind ClassifyPlaybackTarget(const std::wstring& path) {
    if (path.empty()) return PlaybackTargetKind::Empty;
    // URI targets are never passed through local path relocation or durable
    // local-media stores. Validation for opening is handled by the URL parser.
    return path.find(L"://") == std::wstring::npos ? PlaybackTargetKind::LocalFile : PlaybackTargetKind::NetworkStream;
}
struct PlaybackPersistencePolicy {
    bool privateMode;
    bool CanWritePlayback() const { return !privateMode; }
    bool CanWriteLocalTarget(const std::wstring& path) const {
        return !privateMode && ClassifyPlaybackTarget(path) == PlaybackTargetKind::LocalFile;
    }
    bool CanWriteRecentStreams(bool optedIn) const { return !privateMode && optedIn; }
};
} // namespace Aperture
