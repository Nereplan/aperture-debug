#include "ExternalTools.h"
#include "Common.h"

#include <algorithm>
#include <cwctype>
#include <vector>
#include <windows.h>

namespace Aperture {
namespace {
bool IsRootContained(const std::wstring& path, const std::wstring& root) {
    std::wstring full = ResolveAppPath(path, root);
    std::wstring base = ResolveAppPath(root, root);
    std::transform(full.begin(), full.end(), full.begin(), towlower);
    std::transform(base.begin(), base.end(), base.begin(), towlower);
    while (base.size() > 3 && base.back() == L'\\') base.pop_back();
    return full == base || (full.size() > base.size() && full.compare(0, base.size(), base) == 0 && full[base.size()] == L'\\');
}

ExternalToolDiscoveryResult MakeResult(const std::wstring& path, const std::wstring& root) {
    ExternalToolDiscoveryResult result;
    if (!FileExists(path)) return result;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return result;
    result.found = true;
    result.executablePath = ResolveAppPath(path, root);
    result.rootLocal = IsRootContained(result.executablePath, root);
    result.displayPath = result.rootLocal ? ToRelocatablePath(result.executablePath, root) : result.executablePath;
    return result;
}
}

ExternalToolDiscoveryResult ExternalTools::DiscoverYtDlp(bool allowExternalPath) {
    const std::wstring root = GetExeDir();
    for (const auto& candidate : { root + L"\\tools\\yt-dlp.exe", root + L"\\yt-dlp.exe" }) {
        auto result = MakeResult(candidate, root);
        if (result.found) return result;
    }
    if (!allowExternalPath) {
        ExternalToolDiscoveryResult result;
        result.error = L"yt-dlp was not found beside Aperture; external PATH discovery is disabled";
        return result;
    }
    DWORD required = SearchPathW(nullptr, L"yt-dlp.exe", nullptr, 0, nullptr, nullptr);
    if (required == 0 || required > 32768) {
        ExternalToolDiscoveryResult result;
        result.error = L"yt-dlp was not found on PATH";
        return result;
    }
    std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1, L'\0');
    DWORD written = SearchPathW(nullptr, L"yt-dlp.exe", nullptr, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (written == 0 || written >= buffer.size()) {
        ExternalToolDiscoveryResult result;
        result.error = L"yt-dlp PATH resolution failed";
        return result;
    }
    auto result = MakeResult(std::wstring(buffer.data(), written), root);
    if (!result.found) result.error = L"yt-dlp PATH candidate is not a regular file";
    return result;
}

} // namespace Aperture
