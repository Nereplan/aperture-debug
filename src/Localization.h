#pragma once

#include "Common.h"
#include "Json.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <sstream>
#include <type_traits>
#include <memory>
#include <cstdint>

namespace Aperture {

struct LocaleInfo {
    std::string tag;            // Canonical BCP-47-like tag, e.g. "en-US", "es-ES", "de-DE"
    std::string displayName;    // Native display name, e.g. "English (US)", "Español", "Deutsch"
    std::string englishName;    // English display name
    std::string direction = "ltr"; // "ltr" or "rtl"
    bool isLoaded = false;
};

struct PluralEntry { std::string zero, one, two, few; std::string many, other; };

class Localization {
public:
    static Localization& Instance();
    // Initializes the localization service with a root directory (defaults to GetExeDir())
    // and a preferred locale tag (defaults to "en-US").
    bool Initialize(const std::wstring& rootDir = GetExeDir(), const std::string& preferredLocale = "en-US");
    // Sets the active locale tag (e.g. "en-US", "es-ES", "de-DE").
    // If the locale file is missing or invalid, falls back to "en-US" and built-in English.
    bool SetLocale(const std::string& tag);
    // Returns current canonical locale tag (e.g. "en-US").
    std::string GetCurrentLocale() const;
    // Returns current locale metadata.
    // Returns true if active locale reading direction is right-to-left.
    bool IsRtl() const;
    // Unicode Bidirectional Isolation helpers for inserting dynamic LTR paths, URLs, versions, numbers
    static std::wstring BidiIsolateLtr(const std::wstring& str);
    static std::string BidiIsolateLtrUtf8(const std::string& str);
    // RTL isolate (RLI U+2067 / PDI U+2069), mirroring the LTR pair above.
    static std::wstring BidiIsolateRtl(const std::wstring& str);
    // Returns list of all available discovered and built-in locales.
    std::vector<LocaleInfo> GetAvailableLocales() const;
    // Scans the specified locales directory for valid .json locale catalogs.
    std::vector<LocaleInfo> DiscoverLocales(const std::wstring& localesDir) const;
    // String Lookups (Returns localized string; if missing, falls back through hierarchy to English).
    std::wstring Get(const std::string& key) const;
    std::string GetUtf8(const std::string& key) const;
    // Placeholder formatting: replaces {0}, {1}, etc. with provided string arguments.
    std::wstring FormatArgs(const std::string& key, const std::vector<std::wstring>& args) const;
    std::string FormatArgsUtf8(const std::string& key, const std::vector<std::string>& args) const;
    // Plural formatting: chooses plural category according to count and formats with args.
    std::wstring FormatPluralArgs(const std::string& key, int64_t count, const std::vector<std::wstring>& args = {}) const;
    std::string FormatPluralArgsUtf8(const std::string& key, int64_t count, const std::vector<std::string>& args = {}) const;
    std::wstring Format(const std::string& key, const std::vector<std::wstring>& args) const { return FormatArgs(key, args); }
    std::string FormatUtf8(const std::string& key, const std::vector<std::string>& args) const { return FormatArgsUtf8(key, args); }
    std::wstring FormatPlural(const std::string& key, int64_t count, const std::vector<std::wstring>& args = {}) const { return FormatPluralArgs(key, count, args); }
    std::string FormatPluralUtf8(const std::string& key, int64_t count, const std::vector<std::string>& args = {}) const { return FormatPluralArgsUtf8(key, count, args); }
    // Direct pattern formatting helper without key lookup
    static std::wstring FormatPattern(const std::wstring& pattern, const std::vector<std::wstring>& args);
    static std::string FormatPatternUtf8(const std::string& pattern, const std::vector<std::string>& args);
    // Variadic templates for convenient type-safe formatting
    template <typename T, typename... Args> std::wstring Format(const std::string &key, T &&first, Args &&...rest) const {
        std::vector<std::wstring> strArgs;
        strArgs.reserve(1 + sizeof...(Args));
        strArgs.push_back(ToWString(std::forward<T>(first)));
        (strArgs.push_back(ToWString(std::forward<Args>(rest))), ...);
        return FormatArgs(key, strArgs);
    }
    template <typename T, typename... Args> std::string FormatUtf8(const std::string &key, T &&first, Args &&...rest) const {
        std::vector<std::string> strArgs;
        strArgs.reserve(1 + sizeof...(Args));
        strArgs.push_back(ToString(std::forward<T>(first)));
        (strArgs.push_back(ToString(std::forward<Args>(rest))), ...);
        return FormatArgsUtf8(key, strArgs);
    }
    template <typename... Args> std::wstring FormatPlural(const std::string &key, int64_t count, Args &&...args) const {
        std::vector<std::wstring> strArgs;
        strArgs.reserve(sizeof...(Args) + 1);
        strArgs.push_back(std::to_wstring(count));
        (strArgs.push_back(ToWString(std::forward<Args>(args))), ...);
        return FormatPluralArgs(key, count, strArgs);
    }
    template <typename... Args> std::string FormatPluralUtf8(const std::string &key, int64_t count, Args &&...args) const {
        std::vector<std::string> strArgs;
        strArgs.reserve(sizeof...(Args) + 1);
        strArgs.push_back(std::to_string(count));
        (strArgs.push_back(ToString(std::forward<Args>(args))), ...);
        return FormatPluralArgsUtf8(key, count, strArgs);
    }
    // Locale tag validation and canonicalization
    static bool IsValidLocaleTag(const std::string& tag);
    static std::string NormalizeLocaleTag(const std::string& tag);
    static bool IsSafeLocalePath(const std::wstring& path, const std::wstring& localesDir);
    // Diagnostics
    std::string GetLastDiagnostic() const;
    bool HadFallback() const;
    // Complete canonical English catalog map accessor (for validation & testing)
    static const std::unordered_map<std::string, std::string>& GetCanonicalEnglishCatalog();
    static const std::unordered_map<std::string, PluralEntry>& GetCanonicalEnglishPlurals();
    // Catalog parsing and validation helper
    static bool ParseCatalogJson( const std::string& jsonContent, LocaleInfo& outInfo,
        std::unordered_map<std::string, std::string>& outStrings, std::unordered_map<std::string, PluralEntry>& outPlurals,
        std::string& outError );

private:
    Localization();
    ~Localization() = default;
    Localization(const Localization&) = delete;
    Localization& operator=(const Localization&) = delete;
    std::wstring ResolveLocalesDir() const;
    bool LoadLocaleInternal(const std::string& normalizedTag, bool isFallbackChain = false);
    std::string SelectPluralForm(const PluralEntry& entry, int64_t count) const;
    // Type conversion helpers
    template <typename T> static std::wstring ToWString(T &&val) {
        using Decayed = std::decay_t<T>;
        if constexpr (std::is_same_v<Decayed, std::wstring>) {
            return val;
        } else if constexpr (std::is_same_v<Decayed, const wchar_t*> || std::is_same_v<Decayed, wchar_t*>) {
            return val ? std::wstring(val) : std::wstring();
        } else if constexpr (std::is_same_v<Decayed, std::string>) {
            return Utf8ToUtf16(val);
        } else if constexpr (std::is_same_v<Decayed, const char*> || std::is_same_v<Decayed, char*>) {
            return val ? Utf8ToUtf16(val) : std::wstring();
        } else if constexpr (std::is_arithmetic_v<Decayed>) {
            return std::to_wstring(val);
        } else { return Utf8ToUtf16(ToString(std::forward<T>(val))); }
    }
    template<typename T>
    static std::string ToString(T&& val) {
        using Decayed = std::decay_t<T>;
        if constexpr (std::is_same_v<Decayed, std::string>) {
            return val;
        } else if constexpr (std::is_same_v<Decayed, const char*> || std::is_same_v<Decayed, char*>) {
            return val ? std::string(val) : std::string();
        } else if constexpr (std::is_same_v<Decayed, std::wstring>) {
            return Utf16ToUtf8(val);
        } else if constexpr (std::is_same_v<Decayed, const wchar_t*> || std::is_same_v<Decayed, wchar_t*>) {
            return val ? Utf16ToUtf8(val) : std::string();
        } else if constexpr (std::is_arithmetic_v<Decayed>) {
            return std::to_string(val);
        } else { std::ostringstream oss; oss << val; return oss.str(); }
    }
    mutable std::mutex mutex_;
    std::wstring rootDir_;
    std::string currentLocale_ = "en-US";
    LocaleInfo currentLocaleInfo_{ "en-US", "English (US)", "English (US)", "ltr", true };
    // Active loaded catalog
    std::unordered_map<std::string, std::wstring> activeWideStrings_;
    std::unordered_map<std::string, std::string> activeUtf8Strings_;
    std::unordered_map<std::string, PluralEntry> activePlurals_;
    // Fallback loaded catalog (en-US from disk if available)
    std::unordered_map<std::string, std::wstring> fallbackWideStrings_;
    std::unordered_map<std::string, std::string> fallbackUtf8Strings_;
    std::unordered_map<std::string, PluralEntry> fallbackPlurals_;
    mutable std::string lastDiagnostic_;
    bool hadFallback_ = false;
};

// Convenience global namespace
namespace Loc {
    inline std::wstring Tr(const std::string& key) { return Localization::Instance().Get(key); }
    inline std::wstring Get(const std::string& key) { return Localization::Instance().Get(key); }
    inline std::string GetUtf8(const std::string& key) { return Localization::Instance().GetUtf8(key); }
    template <typename... Args> inline std::wstring Format(const std::string &key, Args &&...args) {
        return Localization::Instance().Format(key, std::forward<Args>(args)...);
    }
    template <typename... Args> inline std::string FormatUtf8(const std::string &key, Args &&...args) {
        return Localization::Instance().FormatUtf8(key, std::forward<Args>(args)...);
    }
    template <typename... Args> inline std::wstring FormatPlural(const std::string &key, int64_t count, Args &&...args) {
        return Localization::Instance().FormatPlural(key, count, std::forward<Args>(args)...);
    }
    template <typename... Args> inline std::string FormatPluralUtf8(const std::string &key, int64_t count, Args &&...args) {
        return Localization::Instance().FormatPluralUtf8(key, count, std::forward<Args>(args)...);
    }
    inline bool IsRtl() { return Localization::Instance().IsRtl(); }
    inline std::wstring BidiIsolateLtr(const std::wstring& str) { return Localization::BidiIsolateLtr(str); }
    inline std::string BidiIsolateLtrUtf8(const std::string& str) { return Localization::BidiIsolateLtrUtf8(str); }
    inline std::wstring BidiIsolateRtl(const std::wstring& str) { return Localization::BidiIsolateRtl(str); }
}

} // namespace Aperture
