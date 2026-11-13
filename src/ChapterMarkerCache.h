#pragma once

#include "RenderViewModel.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Aperture {

// Immutable frame snapshots remain valid until a chapter boundary is crossed,
// in either direction. Do not assume chapter times are sorted or unique.
class ChapterMarkerCache {
public:
    std::shared_ptr<const std::vector<TimelineChapterMarker>> Get(
        const std::vector<MpvChapterInfo>& chapters, uint64_t version, double duration, double time) {
        if (snapshot_ && version_ == version && duration_ == duration) {
            if (duration <= 0.0 || (std::isfinite(time) && time >= validFrom_ && time < validUntil_)) {
                return snapshot_;
            }
            // No external caller retains the previous snapshot: update isPassed
            // and the validity interval in place to avoid re-copying chapter titles.
            if (snapshot_.use_count() == 1) {
                auto mutableMarkers = std::const_pointer_cast<std::vector<TimelineChapterMarker>>(snapshot_);
                double validFrom = -std::numeric_limits<double>::infinity();
                double validUntil = std::numeric_limits<double>::infinity();
                for (auto& marker : *mutableMarkers) {
                    marker.isPassed = marker.time <= time;
                    if (marker.isPassed) validFrom = std::max(validFrom, marker.time);
                    else validUntil = std::min(validUntil, marker.time);
                }
                validFrom_ = std::isfinite(time) ? validFrom : 0.0;
                validUntil_ = std::isfinite(time) ? validUntil : 0.0;
                return snapshot_;
            }
        }
        auto markers = std::make_shared<std::vector<TimelineChapterMarker>>();
        double validFrom = -std::numeric_limits<double>::infinity();
        double validUntil = std::numeric_limits<double>::infinity();
        if (duration > 0.0) {
            markers->reserve(std::min<size_t>(chapters.size(), 500));
            for (size_t i = 0; i < chapters.size() && i < 500; ++i) {
                const auto& chapter = chapters[i];
                TimelineChapterMarker marker;
                marker.time = chapter.time;
                marker.normalizedPosition = std::clamp(static_cast<float>(chapter.time / duration), 0.0f, 1.0f);
                marker.title = chapter.title.length() > 80 ? chapter.title.substr(0, 77) + L"..." : chapter.title;
                marker.isPassed = chapter.time <= time;
                if (marker.isPassed) validFrom = std::max(validFrom, chapter.time);
                else validUntil = std::min(validUntil, chapter.time);
                markers->push_back(std::move(marker));
            }
        }
        snapshot_ = std::move(markers);
        version_ = version;
        duration_ = duration;
        // Invalid playback times must not prime an interval for finite times.
        validFrom_ = std::isfinite(time) ? validFrom : 0.0;
        validUntil_ = std::isfinite(time) ? validUntil : 0.0;
        return snapshot_;
    }

private:
    std::shared_ptr<const std::vector<TimelineChapterMarker>> snapshot_;
    uint64_t version_ = UINT64_MAX;
    double duration_ = -1.0;
    double validFrom_ = 0.0, validUntil_ = 0.0;
};

} // namespace Aperture
