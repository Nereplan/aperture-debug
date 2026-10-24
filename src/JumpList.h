#pragma once

// Windows jump list publication for recently played media. The caller is the
// UI thread (COM apartment initialized); this function performs no library or
// settings locking and takes the finished path list as input.
#include <vector>
#include <string>

namespace Aperture {

// Rebuilds the taskbar jump list with a custom "Recent" category containing
// the given paths (missing files are skipped). An empty list publishes a jump
// list without the category, which removes it.
void UpdateJumpList(const std::vector<std::wstring>& recentPaths);

} // namespace Aperture
