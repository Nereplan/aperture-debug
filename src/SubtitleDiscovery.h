#pragma once

#include "Common.h"
#include <string>
#include <vector>
#include <cstdint>

namespace Aperture {

enum class SubtitleEncoding { Unknown, Utf8, Utf8Bom, Utf16LE, Utf16BE, Utf32LE, Utf32BE, Ansi };

struct DiscoveredSubtitle {
    std::wstring filePath, title, language;
    int matchScore = 0;
    SubtitleEncoding encoding = SubtitleEncoding::Unknown;
    bool isSubfolder = false;
    uint64_t fileSizeBytes = 0;
};

struct SubtitlePreferences {
    int primaryTrackIndex = -1, secondaryTrackIndex = -1;
    std::wstring primaryTrackPath, secondaryTrackPath;
    int primaryDelayMs = 0, secondaryDelayMs = 0;
    bool primaryVisible = true, secondaryVisible = true;
    std::string primaryPosition = "bottom";
    std::string secondaryPosition = "top";
    std::string primaryFontSize = "46";
    std::string secondaryFontSize = "38";
    std::string primaryColor = "#FFFFFF";
    std::string secondaryColor = "#FFFF00";
    std::string SerializeJson() const;
    bool DeserializeJson(const std::string& jsonStr);
};

class SubtitleDiscovery {
public:
    static constexpr size_t kMaxSubtitleInputBytes = 32 * 1024 * 1024; // 32 MB
    static constexpr size_t kMaxDiscoveredFiles = 128;
    static constexpr int kMinSubtitleDelayMs = -600000; // -10 minutes
    static constexpr int kMaxSubtitleDelayMs = 600000;  // +10 minutes
    // Normalizes paths, resolving relative elements and rejecting invalid/reserved paths.
    static std::wstring NormalizePath(const std::wstring& path);
    // Verifies that candidatePath is strictly contained within basePath (prevents directory traversal).
    static bool IsPathContained(const std::wstring& basePath, const std::wstring& candidatePath);
    // Checks if the filename contains Windows reserved DOS device names.
    static bool IsReservedDeviceName(const std::wstring& pathOrName);
    // Auto-discovers external subtitle files for a given media file path within the media directory
    // and bounded conventional subfolders ('subs', 'subtitles') without recursive escape.
    static std::vector<DiscoveredSubtitle> DiscoverSubtitles(const std::wstring& mediaPath);
    // Detects subtitle encoding via BOM and content heuristics.
    static SubtitleEncoding DetectEncoding(const uint8_t* data, size_t size);
    static SubtitleEncoding DetectEncoding(const std::string& bytes);
    // Robustly decodes subtitle bytes into normalized UTF-8 string according to detected encoding.
    static std::string DecodeSubtitleBytes(const std::string& bytes, SubtitleEncoding* outEncoding = nullptr);
    // Extracts and normalizes language name from filename or suffix (e.g. .en.srt -> "English").
    static std::wstring ParseLanguageFromFileName(const std::wstring& fileName);
    // Computes match score for a candidate subtitle file relative to media file.
    static int CalculateMatchScore(const std::wstring& mediaFileName, const std::wstring& subtitleFileName, bool inSubfolder);
    // Bounded delay clamping helper.
    static int ClampDelayMs(int delayMs);
    // Path persistence helpers (root-relative where safe, no escaping).
    static std::wstring ToRootRelativePath(const std::wstring& fullPath, const std::wstring& rootDir);
    static std::wstring FromRootRelativePath(const std::wstring& relPath, const std::wstring& rootDir);
};

} // namespace Aperture
