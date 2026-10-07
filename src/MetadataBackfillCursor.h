#pragma once

#include "MediaLibrary.h"

namespace Aperture {

// One forward pass for a stable catalog, copying only the next bounded batch.
// External catalog changes restart the pass; this pass's own metadata merge
// can advance the expected revision without rescanning its processed prefix.
class MetadataBackfillCursor {
public:
    void Reset() { next_ = examined_ = 0; catalog_ = (std::numeric_limits<uint64_t>::max)(); }
    std::vector<MediaItem> Take(const MediaLibrary& library, const MetadataBackfillOptions& options) {
        const auto catalog = library.GetRevisions().catalog;
        if (catalog_ != catalog) { next_ = 0; catalog_ = catalog; }
        const size_t cap = options.maxItems ? options.maxItems : 32;
        const int64_t now = options.nowTimestamp > 0 ? options.nowTimestamp : GetCurrentTimestampSec();
        std::vector<MediaItem> batch;
        batch.reserve((std::min)(cap, library.mediaItems.size()));
        while (next_ < library.mediaItems.size() && batch.size() < cap) {
            const auto& item = library.mediaItems[next_++];
            ++examined_;
            if (MediaLibrary::CanAttemptMetadataBackfill(item, options, now)) batch.push_back(item);
        }
        return batch;
    }
    void AcceptOwnChanges(uint64_t before, uint64_t after) { if (catalog_ == before) catalog_ = after; }
    size_t GetExaminedCount() const { return examined_; }
private:
    size_t next_ = 0, examined_ = 0;
    uint64_t catalog_ = (std::numeric_limits<uint64_t>::max)();
};

} // namespace Aperture
