#pragma once

#include "Common.h"
#include "SubtitleDiscovery.h"
#include <string>
#include <vector>
#include <memory>

namespace Aperture {

struct SubtitleCue { double startTimeSec = 0.0; double endTimeSec = 0.0; std::wstring text; };

struct SubtitleTrack {
    std::string id;
    std::wstring name, filePath;
    std::vector<SubtitleCue> cues;
    bool isEmbedded = false;
};

class SubtitleEngine {
public:
    static constexpr size_t kMaxSubtitleInputBytes = SubtitleDiscovery::kMaxSubtitleInputBytes; // 32 MB
    static constexpr size_t kMaxSubtitleCueCount = 250000;                                     // Max 250,000 cues per track
    static constexpr size_t kMaxCueTextBytes = 128 * 1024;                                     // Max 128 KB per cue text
    static constexpr size_t kMaxFormattingWorkUnits = 65536;                                   // Work limit for clean/entity operations per cue
    std::vector<SubtitleTrack> tracks;
    // Bump whenever tracks or their cues change, so text and lookup indexes
    // cannot alias a rebuilt track (including in-place cue edits).
    uint64_t tracksGeneration = 0;
    struct RenderedTextCacheSlot {
        const SubtitleTrack* track = nullptr;
        int delayMs = -1;
        uint64_t generation = 0;
        double validFromSec = 0.0, validUntilSec = 0.0;
        std::wstring text;
        const SubtitleCue* indexedCues = nullptr;
        size_t indexedCueCount = 0;
        // Maximum cue end per subtree, with 32 cues per leaf. Allocated only
        // for selected tracks larger than 64 cues, and reused across seeks.
        size_t cueBlockBase = 0;
        std::vector<double> cueEndTree;
    };
    mutable RenderedTextCacheSlot primaryTextCache;
    mutable RenderedTextCacheSlot secondaryTextCache;
    int activeTrackIndex = -1;    // -1 = off (synchronized with primaryTrackIndex)
    int primaryTrackIndex = -1;   // -1 = off
    int secondaryTrackIndex = -1; // -1 = off
    bool primaryVisible = true, secondaryVisible = true;
    int primaryDelayMs = 0;       // Clamped to [-600000, 600000]
    int secondaryDelayMs = 0;     // Clamped to [-600000, 600000]
    std::string primaryPosition = "bottom";   // "bottom", "top", "elevated"
    std::string secondaryPosition = "top";    // "top", "bottom", "elevated"
    std::string primaryFontSize = "46";
    std::string secondaryFontSize = "38";
    std::string primaryColor = "#FFFFFF";
    std::string secondaryColor = "#FFFF00";
    SubtitleEngine();
    void Clear();
    void LoadSubtitlesForMedia(const std::wstring& mediaPath);
    bool LoadSubtitleFile(const std::wstring& filePath, const std::wstring& trackName = L"");
    // Primary track methods (compatible with single-track API)
    void SelectTrack(int index);
    void SelectPrimaryTrack(int index);
    void CyclePrimaryTrack();
    const SubtitleTrack* GetActiveTrack() const;
    const SubtitleTrack* GetPrimaryTrack() const;
    int GetPrimaryTrackIndex() const { return primaryTrackIndex; }
    // Secondary track methods
    void SelectSecondaryTrack(int index);
    void CycleSecondaryTrack();
    const SubtitleTrack* GetSecondaryTrack() const;
    int GetSecondaryTrackIndex() const { return secondaryTrackIndex; }
    // Visibility
    void SetPrimaryVisibility(bool visible) { primaryVisible = visible; }
    void SetSecondaryVisibility(bool visible) { secondaryVisible = visible; }
    bool IsPrimaryVisible() const { return primaryVisible; }
    bool IsSecondaryVisible() const { return secondaryVisible; }
    // Delays
    void SetPrimaryDelayMs(int delayMs);
    void SetSecondaryDelayMs(int delayMs);
    int GetPrimaryDelayMs() const { return primaryDelayMs; }
    int GetSecondaryDelayMs() const { return secondaryDelayMs; }
    // Text rendering query
    std::wstring GetCurrentSubtitleText(double currentPlaybackTimeSec, int delayMs = 0) const;
    std::wstring GetPrimarySubtitleText(double currentPlaybackTimeSec) const;
    std::wstring GetSecondarySubtitleText(double currentPlaybackTimeSec) const;
    void GetDualSubtitleText(double currentPlaybackTimeSec, std::wstring& outPrimary, std::wstring& outSecondary) const;
    int GetTrackCount() const { return static_cast<int>(tracks.size()); }
    // Preferences export & import
    SubtitlePreferences ExportPreferences(const std::wstring& rootDir = L"") const;
    void ImportPreferences(const SubtitlePreferences& prefs, const std::wstring& rootDir = L"");
    // Static parsing & decoding utilities
    static bool ParseSrt(const std::string& content, std::vector<SubtitleCue>& outCues);
    static bool ParseVtt(const std::string& content, std::vector<SubtitleCue>& outCues);
    static bool ParseAss(const std::string& content, std::vector<SubtitleCue>& outCues);
    static bool ParseSami(const std::string& content, std::vector<SubtitleCue>& outCues);
    static bool ParseMicroDvd(const std::string& content, std::vector<SubtitleCue>& outCues);
    static bool ParseVPlayer(const std::string& content, std::vector<SubtitleCue>& outCues);
    static std::string DecodeSubtitleBytes(const std::string& bytes);
    static double ParseTimestamp(const std::string& timeStr);
    static std::wstring CleanSubtitleFormatting(const std::string& rawText);
};

} // namespace Aperture
