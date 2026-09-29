#pragma once

// User-facing duration and file-size formatting.

#include "AppIncludes.h"
#include <cstdint>
#include <string>

namespace Aperture {

// --- Formatters ---
inline std::wstring FormatDuration(double totalSeconds) {
    if (totalSeconds < 0 || !std::isfinite(totalSeconds)) totalSeconds = 0;
    int totalSec = (int)std::round(totalSeconds);
    int hours = totalSec / 3600;
    int minutes = (totalSec % 3600) / 60;
    int seconds = totalSec % 60;
    wchar_t buf[32];
    if (hours > 0) { swprintf_s(buf, L"%d:%02d:%02d", hours, minutes, seconds); } else { swprintf_s(buf, L"%02d:%02d", minutes, seconds); }
    return buf;
}

inline std::wstring FormatFileSize(uint64_t bytes) {
    wchar_t buf[32];
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
        swprintf_s(buf, L"%.2f GB", gb);
    } else if (bytes >= 1024ULL * 1024ULL) {
        double mb = (double)bytes / (1024.0 * 1024.0);
        swprintf_s(buf, L"%.1f MB", mb);
    } else if (bytes >= 1024ULL) {
        double kb = (double)bytes / 1024.0;
        swprintf_s(buf, L"%.0f KB", kb);
    } else { swprintf_s(buf, L"%llu B", bytes); }
    return buf;
}

inline std::wstring FormatFileSizeStr(uint64_t bytes) {
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
        wchar_t buf[32];
        swprintf_s(buf, L"%.1f GB", gb);
        return buf;
    }
    double mb = (double)bytes / (1024.0 * 1024.0);
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f MB", mb);
    return buf;
}

inline std::wstring FormatDurationHumanStr(double totalSec) {
    int s = (int)totalSec;
    int h = s / 3600;
    int m = (s % 3600) / 60;
    if (h > 0) { return std::to_wstring(h) + L"h " + std::to_wstring(m) + L"m"; }
    return std::to_wstring(m) + L"m";
}

inline int64_t GetCurrentTimestampSec() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

} // namespace Aperture
