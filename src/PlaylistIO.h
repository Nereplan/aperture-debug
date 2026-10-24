#pragma once

#include "Common.h"
#include "MediaLibrary.h"
#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace Aperture {

enum class PlaylistFormat { Auto = 0, M3U, M3U8, PLS, XSPF, ASX, WPL };

enum class PlaylistIoErrorCode {
    Success = 0, FileNotFound, FileOpenFailed, FileReadFailed, FileWriteFailed, FileTooLarge, LineLimitExceeded, ItemLimitExceeded,
    LineTooLong, EmptyPlaylist, InvalidHeader, InvalidPath, AtomicRenameFailed, DirectoryCreationFailed
};

struct PlaylistEntry {
    std::wstring location;      // Resolved file path or URL string
    std::wstring title;         // Optional title from #EXTINF or TitleN
    double durationSec = -1.0;  // Duration in seconds (-1.0 if unknown or stream)
    bool isUrl = false;         // True if http://, https://, rtsp://, etc.
    bool fileExists = true;     // False if local file is missing on disk
    std::string rawMetadata;    // Raw comment or extra metadata if present
};

struct PlaylistParseOptions {
    size_t maxFileSizeBytes = 10 * 1024 * 1024; // 10 MB max file size
    size_t maxLineCount = 100000;              // 100k lines max
    size_t maxItemCount = 10000;               // 10k items max
    size_t maxLineLength = 32768;              // 32 KB per line max
    bool resolveRelativePaths = true;          // Resolve relative paths against playlist folder
    bool checkFileExistence = true;            // Check local file existence on disk
    bool allowEmptyPlaylist = false;           // If false, empty playlist is treated as error
    bool preserveDuplicates = true;            // Preserve duplicate entries in order
};

struct PlaylistExportOptions {
    PlaylistFormat format = PlaylistFormat::M3U8;
    bool writeUtf8Bom = false;                 // Write UTF-8 BOM (0xEF, 0xBB, 0xBF)
    bool useRelativePathsWhereSafe = true;     // Write relative paths if under basePath
    std::wstring basePath = L"";               // Base directory for relative paths (empty = export file dir)
    bool writeExtendedMetadata = true;         // Write #EXTINF / TitleN / LengthN
    std::wstring playlistTitle = L"";          // XSPF <title> element content (empty = omit)
    bool collisionSafe = false;                // Auto-generate name (1).ext if target exists
    bool atomicWrite = true;                   // Write to temp file then atomic rename
};

struct PlaylistParseResult {
    bool success = true;
    PlaylistIoErrorCode errorCode = PlaylistIoErrorCode::Success;
    DWORD win32Error = 0;
    std::string message = "OK";
    std::wstring sourcePath;
    PlaylistFormat detectedFormat = PlaylistFormat::Auto;
    std::wstring playlistTitle;
    std::vector<PlaylistEntry> entries;
    size_t totalLinesParsed = 0, validItemCount = 0, missingItemCount = 0, urlItemCount = 0;
    size_t duplicateItemCount = 0;
    std::vector<std::string> warnings;
    bool is_ok() const { return success; }
    explicit operator bool() const { return success; }
    static PlaylistParseResult SuccessResult( const std::wstring& srcPath, PlaylistFormat fmt, std::vector<PlaylistEntry> items,
        const std::wstring &title = L"") {
        PlaylistParseResult r;
        r.success = true;
        r.errorCode = PlaylistIoErrorCode::Success;
        r.sourcePath = srcPath;
        r.detectedFormat = fmt;
        r.playlistTitle = title;
        r.entries = std::move(items);
        return r;
    }
    static PlaylistParseResult ErrorResult( PlaylistIoErrorCode code, const std::wstring& srcPath, const std::string& msg,
        DWORD win32Err = 0) {
        PlaylistParseResult r;
        r.success = false;
        r.errorCode = code;
        r.sourcePath = srcPath;
        r.message = msg;
        r.win32Error = win32Err;
        return r;
    }
};

struct PlaylistExportResult {
    bool success = true;
    PlaylistIoErrorCode errorCode = PlaylistIoErrorCode::Success;
    DWORD win32Error = 0;
    std::string message = "OK";
    std::wstring exportedPath;
    PlaylistFormat format = PlaylistFormat::M3U8;
    size_t itemsExported = 0;
    bool is_ok() const { return success; }
    explicit operator bool() const { return success; }
    static PlaylistExportResult SuccessResult(const std::wstring &outPath, PlaylistFormat fmt, size_t count) {
        PlaylistExportResult r;
        r.success = true;
        r.errorCode = PlaylistIoErrorCode::Success;
        r.exportedPath = outPath;
        r.format = fmt;
        r.itemsExported = count;
        return r;
    }
    static PlaylistExportResult ErrorResult( PlaylistIoErrorCode code, const std::wstring& outPath, const std::string& msg,
        DWORD win32Err = 0) {
        PlaylistExportResult r;
        r.success = false;
        r.errorCode = code;
        r.exportedPath = outPath;
        r.message = msg;
        r.win32Error = win32Err;
        return r;
    }
};

class PlaylistIO {
public:
    // Format detection
    static PlaylistFormat DetectFormatFromExtension(const std::wstring& path);
    static PlaylistFormat DetectFormatFromContent(const std::string& content);
    static bool IsUrl(const std::wstring& path);
    // Path helpers
    static std::wstring NormalizeSeparators(const std::wstring& path);
    static std::wstring EnsureExtendedPath(const std::wstring& path);
    static std::wstring ResolveEntryLocation( const std::wstring& entryPath, const std::wstring& playlistDir, bool resolveRelative = true);
    static std::wstring MakeRelativePathWhereSafe( const std::wstring& fullPath, const std::wstring& baseDir);
    static std::wstring GenerateCollisionSafePath(const std::wstring& targetPath);
    // Append paths that are not already present, using the same ordinal
    // case-insensitive equality used by Windows path lookups.
    static size_t AppendUniquePathsOrdinalIgnoreCase(std::vector<std::wstring>& existing,
        const std::vector<std::wstring>& candidates);
    // Text & Encoding helpers
    static std::string DecodeToUtf8( const char* data, size_t size, bool& outHasBom, std::string& outEncodingName);
    // Core parsers from in-memory string
    static PlaylistParseResult ParseM3UContent( const std::string& utf8Content, const std::wstring& playlistDir = L"",
        const PlaylistParseOptions& options = {});
    static PlaylistParseResult ParsePLSContent( const std::string& utf8Content, const std::wstring& playlistDir = L"",
        const PlaylistParseOptions& options = {});
    static PlaylistParseResult ParseXSPFContent( const std::string& utf8Content, const std::wstring& playlistDir = L"",
        const PlaylistParseOptions& options = {});
    static PlaylistParseResult ParseASXContent( const std::string& utf8Content, const std::wstring& playlistDir = L"",
        const PlaylistParseOptions& options = {});
    static PlaylistParseResult ParseWPLContent( const std::string& utf8Content, const std::wstring& playlistDir = L"",
        const PlaylistParseOptions& options = {});
    static PlaylistParseResult ParseContent( const std::string& rawBytes, PlaylistFormat format = PlaylistFormat::Auto,
        const std::wstring& playlistDir = L"", const PlaylistParseOptions& options = {});
    // High-level File Import
    static PlaylistParseResult ImportPlaylistFile( const std::wstring& filePath, const PlaylistParseOptions& options = {});
    // Core serializers to string
    static std::string SerializeM3U( const std::vector<PlaylistEntry>& entries, const PlaylistExportOptions& options);
    static std::string SerializePLS( const std::vector<PlaylistEntry>& entries, const PlaylistExportOptions& options);
    static std::string SerializeXSPF( const std::vector<PlaylistEntry>& entries, const std::wstring& playlistTitle,
        const PlaylistExportOptions& options);
    // High-level File Export
    static PlaylistExportResult ExportPlaylistFile( const std::wstring& destinationPath, const std::vector<PlaylistEntry>& entries,
        const PlaylistExportOptions& options = {});
    static PlaylistExportResult ExportPlaylistFile( const std::wstring& destinationPath, const Playlist& playlist,
        const PlaylistExportOptions& options = {});
    static PlaylistExportResult ExportPlaylistFile( const std::wstring& destinationPath, const std::vector<std::wstring>& itemPaths,
        const std::wstring& playlistTitle = L"", const PlaylistExportOptions& options = {});
    // Interop with MediaLibrary::Playlist model
    static std::vector<PlaylistEntry> FromMediaLibraryPlaylist( const Playlist& playlist);
};

} // namespace Aperture
