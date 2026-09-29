#include "JumpList.h"
#include "Common.h"
#include "NetworkStream.h"

#include <algorithm>
#include <unordered_set>
#include <windows.h>
#include <shobjidl.h>

namespace Aperture {
// Portable containment note: shell COM helpers were removed with the JumpList file writes. ProjectRecentLocalMedia remains as a pure
// in-memory projection for the hub UI.

std::vector<JumpListItem> JumpListController::ProjectRecentLocalMedia( const std::vector<HistoryRecord>& history, size_t limit) {
    limit = std::min<size_t>(limit, 20);
    std::vector<const HistoryRecord*> ordered;
    ordered.reserve(history.size());
    for (const auto& entry : history) {
        if (entry.filePath.empty() || NetworkStreamValidator::IsValid(entry.filePath) || !FileExists(entry.filePath)) continue;
        ordered.push_back(&entry);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto *a, const auto *b) { return a->watchedTimestamp > b->watchedTimestamp; });
    std::unordered_set<std::wstring> seen;
    std::vector<JumpListItem> result;
    for (const auto* entry : ordered) {
        std::wstring key = ResolveAppPath(entry->filePath);
        std::transform(key.begin(), key.end(), key.begin(), towlower);
        if (!seen.insert(key).second) continue;
        result.push_back({entry->filePath, entry->title.empty() ? GetFileNameFromPath(entry->filePath) : entry->title});
        if (result.size() == limit) break;
    }
    return result;
}

bool JumpListController::Update(const std::vector<HistoryRecord>& history) {
    // Portable containment: the Windows JumpList lives under
    // %APPDATA%\Microsoft\Windows\Recent\CustomDestinations, outside the
    // application folder. Strict portable mode keeps zero durable state outside the root, so this is intentionally a no-op. Recent-media
    // projection stays available in-memory via ProjectRecentLocalMedia.
    (void)history;
    return false;
}

void JumpListController::Clear() {
    // Portable containment: never touch %APPDATA% even to delete. No-op.
}

} // namespace Aperture
