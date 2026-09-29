#pragma once

#include "MediaLibrary.h"
#include <string>
#include <vector>

namespace Aperture {

struct JumpListItem { std::wstring filePath; std::wstring title; };

class JumpListController {
public:
    static std::vector<JumpListItem> ProjectRecentLocalMedia( const std::vector<HistoryRecord>& history, size_t limit = 10);
    bool Update(const std::vector<HistoryRecord>& history);
    void Clear();
};

} // namespace Aperture
