#include "Subtitles.h"
#include <fstream>
#include <sstream>
#include <set>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <limits>

namespace Aperture {

namespace {

std::string TrimAscii(const std::string& value) {
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    return value.substr(first, last - first);
}

bool StartsWithInsensitive(const std::string& value, const char* prefix) {
    if (!prefix) return false;
    size_t length = std::strlen(prefix);
    if (value.size() < length) return false;
    for (size_t i = 0; i < length; ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
    }
    return true;
}

void SortCues(std::vector<SubtitleCue>& cues) {
    if (cues.size() > SubtitleEngine::kMaxSubtitleCueCount) { cues.resize(SubtitleEngine::kMaxSubtitleCueCount); }
    std::stable_sort(cues.begin(), cues.end(), [](const SubtitleCue& a, const SubtitleCue& b) {
        double aStart = std::isfinite(a.startTimeSec) ? a.startTimeSec : 0.0;
        double bStart = std::isfinite(b.startTimeSec) ? b.startTimeSec : 0.0;
        if (aStart != bStart) return aStart < bStart;
        double aEnd = std::isfinite(a.endTimeSec) ? a.endTimeSec : 0.0;
        double bEnd = std::isfinite(b.endTimeSec) ? b.endTimeSec : 0.0;
        return aEnd < bEnd;
    });
}

bool ParseTimingLine(const std::string& line, double& start, double& end, double (*parseTimestamp)(const std::string&)) {
    size_t arrow = line.find("-->");
    if (arrow == std::string::npos) return false;
    std::string left = TrimAscii(line.substr(0, arrow));
    std::string right = TrimAscii(line.substr(arrow + 3));
    if (left.empty() || right.empty()) return false;
    size_t settingEnd = right.find_first_of(" \t");
    if (settingEnd != std::string::npos) right.resize(settingEnd);
    start = parseTimestamp ? parseTimestamp(left) : -1.0;
    end = parseTimestamp ? parseTimestamp(right) : -1.0;
    return (start >= 0.0 && end >= 0.0 && std::isfinite(start) && std::isfinite(end));
}

std::wstring GetTrackTextInternal(const SubtitleTrack* track, double currentPlaybackTimeSec, int delayMs, double* validUntilSec);

// Renders through a per-slot cache: playback redraws query the same track at
// frame rate, yet the rendered text can only change at a cue boundary (a cue
// starting or ending). The render reports the next such boundary so the cache
// can serve identical frames without re-rendering.
std::wstring GetTrackTextCached(const SubtitleTrack* track, double currentPlaybackTimeSec, int delayMs,
                                uint64_t generation, SubtitleEngine::RenderedTextCacheSlot& slot) {
    if (!track) return L"";
    const int clampedDelayMs = SubtitleDiscovery::ClampDelayMs(delayMs);
    double delaySec = static_cast<double>(clampedDelayMs) / 1000.0;
    double adjustedTime = currentPlaybackTimeSec - delaySec;
    if (std::isfinite(adjustedTime) && adjustedTime >= 0.0 && slot.track == track && slot.delayMs == clampedDelayMs &&
        slot.generation == generation && adjustedTime >= slot.validFromSec && adjustedTime < slot.validUntilSec) {
        return slot.text;
    }
    double validUntil = 0.0;
    std::wstring rendered = GetTrackTextInternal(track, currentPlaybackTimeSec, clampedDelayMs, &validUntil);
    slot.track = track;
    slot.delayMs = clampedDelayMs;
    slot.generation = generation;
    slot.validFromSec = std::isfinite(adjustedTime) && adjustedTime >= 0.0 ? adjustedTime : 0.0;
    slot.validUntilSec = validUntil;
    slot.text = rendered;
    return rendered;
}

std::wstring GetTrackTextInternal(const SubtitleTrack* track, double currentPlaybackTimeSec, int delayMs, double* validUntilSec) {
    if (validUntilSec) { *validUntilSec = 0.0; }
    if (!track || track->cues.empty() || !std::isfinite(currentPlaybackTimeSec)) return L"";
    double delaySec = static_cast<double>(SubtitleDiscovery::ClampDelayMs(delayMs)) / 1000.0;
    double adjustedTime = currentPlaybackTimeSec - delaySec;
    if (adjustedTime < 0.0 || !std::isfinite(adjustedTime)) return L"";
    auto upper = std::upper_bound(track->cues.begin(), track->cues.end(), adjustedTime,
        [](double time, const SubtitleCue& cue) { return time < cue.startTimeSec; });
    // The text is constant until the next cue starts (window grows) or an
    // active cue ends (window content changes), whichever comes first.
    double nextBoundary = upper != track->cues.end() ? upper->startTimeSec : std::numeric_limits<double>::infinity();
    std::wstring rendered;
    std::set<std::wstring> seen;
    size_t renderedCount = 0;
    // Cues before upper all start at or before this time. Walk them once;
    // nested/overlapping cues can still be active far before the nearest start.
    for (auto it = track->cues.begin(); it != upper; ++it) {
        if (adjustedTime < it->startTimeSec || adjustedTime > it->endTimeSec || it->text.empty()) continue;
        nextBoundary = std::min(nextBoundary, it->endTimeSec);
        if (!seen.insert(it->text).second) continue;
        if (!rendered.empty()) rendered += L'\n';
        rendered += it->text;
        ++renderedCount;
        if (renderedCount >= 64 || rendered.size() >= SubtitleEngine::kMaxCueTextBytes) break;
    }
    if (nextBoundary <= adjustedTime) { nextBoundary = std::nextafter(adjustedTime, std::numeric_limits<double>::infinity()); }
    if (validUntilSec) { *validUntilSec = nextBoundary; }
    return rendered;
}

} // namespace

std::string SubtitleEngine::DecodeSubtitleBytes(const std::string& bytes) { return SubtitleDiscovery::DecodeSubtitleBytes(bytes); }

SubtitleEngine::SubtitleEngine() : activeTrackIndex(-1), primaryTrackIndex(-1), secondaryTrackIndex(-1),
      primaryVisible(true), secondaryVisible(true), primaryDelayMs(0), secondaryDelayMs(0),
      primaryPosition("bottom"), secondaryPosition("top"), primaryFontSize("46"), secondaryFontSize("38"),
      primaryColor("#FFFFFF"), secondaryColor("#FFFF00") {}

void SubtitleEngine::Clear() {
    ++tracksGeneration;
    tracks.clear();
    activeTrackIndex = -1;
    primaryTrackIndex = -1;
    secondaryTrackIndex = -1;
    primaryDelayMs = 0;
    secondaryDelayMs = 0;
    primaryVisible = true;
    secondaryVisible = true;
}

const SubtitleTrack* SubtitleEngine::GetActiveTrack() const { return GetPrimaryTrack(); }

const SubtitleTrack* SubtitleEngine::GetPrimaryTrack() const {
    if (primaryTrackIndex >= 0 && primaryTrackIndex < static_cast<int>(tracks.size())) { return &tracks[primaryTrackIndex]; }
    return nullptr;
}

const SubtitleTrack* SubtitleEngine::GetSecondaryTrack() const {
    if (secondaryTrackIndex >= 0 && secondaryTrackIndex < static_cast<int>(tracks.size())) { return &tracks[secondaryTrackIndex]; }
    return nullptr;
}

void SubtitleEngine::SelectTrack(int index) { SelectPrimaryTrack(index); }

void SubtitleEngine::SelectPrimaryTrack(int index) {
    if (index >= 0 && index < static_cast<int>(tracks.size())) {
        primaryTrackIndex = index;
        activeTrackIndex = index;
    } else { primaryTrackIndex = -1; activeTrackIndex = -1; }
}

void SubtitleEngine::SelectSecondaryTrack(int index) {
    if (index >= 0 && index < static_cast<int>(tracks.size())) { secondaryTrackIndex = index; } else { secondaryTrackIndex = -1; }
}

void SubtitleEngine::CyclePrimaryTrack() {
    if (tracks.empty()) { primaryTrackIndex = -1; activeTrackIndex = -1; return; }
    primaryTrackIndex = (primaryTrackIndex + 1) % (static_cast<int>(tracks.size()) + 1);
    if (primaryTrackIndex == static_cast<int>(tracks.size())) { primaryTrackIndex = -1; }
    activeTrackIndex = primaryTrackIndex;
}

void SubtitleEngine::CycleSecondaryTrack() {
    if (tracks.empty()) { secondaryTrackIndex = -1; return; }
    secondaryTrackIndex = (secondaryTrackIndex + 1) % (static_cast<int>(tracks.size()) + 1);
    if (secondaryTrackIndex == static_cast<int>(tracks.size())) { secondaryTrackIndex = -1; }
}

void SubtitleEngine::SetPrimaryDelayMs(int delayMs) { primaryDelayMs = SubtitleDiscovery::ClampDelayMs(delayMs); }

void SubtitleEngine::SetSecondaryDelayMs(int delayMs) { secondaryDelayMs = SubtitleDiscovery::ClampDelayMs(delayMs); }

double SubtitleEngine::ParseTimestamp(const std::string& timeStr) {
    if (timeStr.empty() || timeStr.size() > 256) return -1.0;
    std::string value = TrimAscii(timeStr);
    if (value.empty()) return -1.0;
    for (char c : value) {
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != ':' && c != '.' && c != ',' && c != ' ' && c != '\t') { return -1.0; }
    }
    size_t firstDigit = value.find_first_of("0123456789");
    if (firstDigit == std::string::npos) return -1.0;
    value = value.substr(firstDigit);
    size_t tokenEnd = value.find_first_of(" \t\r\n");
    if (tokenEnd != std::string::npos) value.resize(tokenEnd);
    for (char& ch : value) if (ch == ',') ch = '.';
    double parts[4] = { 0.0, 0.0, 0.0, 0.0 };
    size_t partCount = 0, begin = 0;
    while (begin <= value.size()) {
        if (partCount >= 3) return -1.0;
        size_t colon = value.find(':', begin);
        std::string part = value.substr(begin, colon == std::string::npos ? std::string::npos : colon - begin);
        if (part.empty() || part.size() > 64) return -1.0;
        char* endPtr = nullptr;
        double number = std::strtod(part.c_str(), &endPtr);
        if (!endPtr || *endPtr != '\0' || !std::isfinite(number) || number < 0.0 || number > 1e9) return -1.0;
        parts[partCount++] = number;
        if (colon == std::string::npos) break;
        begin = colon + 1;
    }
    if (partCount == 0 || partCount > 3) return -1.0;
    if (partCount == 1) return parts[0];
    if (partCount == 2) return parts[0] * 60.0 + parts[1];
    return parts[0] * 3600.0 + parts[1] * 60.0 + parts[2];
}

std::wstring SubtitleEngine::CleanSubtitleFormatting(const std::string& rawText) {
    if (rawText.empty() || rawText.size() > kMaxCueTextBytes) return L"";
    const size_t sourceLen = rawText.size();
    std::string cleaned;
    cleaned.reserve(sourceLen);
    size_t workUnits = 0;
    for (size_t i = 0; i < sourceLen && workUnits < kMaxFormattingWorkUnits; ++i) {
        ++workUnits;
        char ch = rawText[i];
        if (ch == '<') {
            size_t close = rawText.find('>', i + 1);
            if (close != std::string::npos && (close - i) <= 4096) {
                size_t tagLen = close - i + 1;
                if (tagLen >= 4 && std::tolower(static_cast<unsigned char>(rawText[i + 1])) == 'b' &&
                    std::tolower(static_cast<unsigned char>(rawText[i + 2])) == 'r') {
                    char nextChar = rawText[i + 3];
                    if (nextChar == '>' || nextChar == '/' || nextChar == ' ' || nextChar == '\t') { cleaned.push_back('\n'); }
                }
                i = close;
                continue;
            }
        }
        if (ch == '{') {
            size_t close = rawText.find('}', i + 1);
            if (close != std::string::npos && (close - i) <= 4096) { i = close; continue; }
        }
        if (ch == '\\' && i + 1 < sourceLen) {
            char escaped = rawText[i + 1];
            if (escaped == 'N' || escaped == 'n') { cleaned.push_back('\n'); ++i; continue; }
            if (escaped == 'h') { cleaned.push_back(' '); ++i; continue; }
        }
        cleaned.push_back(ch);
    }
    const std::pair<const char *, const char *> entities[] = {{"&nbsp;", " "},  {"&amp;", "&"}, {"&lt;", "<"},  {"&gt;", ">"},
                                                              {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"}};
    for (const auto& entity : entities) {
        size_t pos = 0;
        const size_t needleLength = std::strlen(entity.first);
        const size_t replLength = std::strlen(entity.second);
        size_t entityReplacements = 0;
        while ((pos = cleaned.find(entity.first, pos)) != std::string::npos && entityReplacements < 1000 &&
               workUnits < kMaxFormattingWorkUnits) {
            ++workUnits;
            ++entityReplacements;
            cleaned.replace(pos, needleLength, entity.second);
            pos += replLength;
        }
    }
    cleaned = TrimAscii(cleaned);
    if (cleaned.empty()) return L"";
    if (cleaned.size() > kMaxCueTextBytes) cleaned.resize(kMaxCueTextBytes);
    return Utf8ToUtf16(cleaned);
}

bool SubtitleEngine::ParseSrt(const std::string& content, std::vector<SubtitleCue>& outCues) {
    outCues.clear();
    if (content.empty() || content.size() > kMaxSubtitleInputBytes) return false;
    std::istringstream stream(content);
    std::string line;
    SubtitleCue current;
    bool hasTiming = false, skipBlock = false;
    size_t totalCuesAdded = 0;
    auto flush = [&]() {
        if (!hasTiming || current.text.empty()) { current = SubtitleCue(); hasTiming = false; return; }
        if (!std::isfinite(current.startTimeSec) || !std::isfinite(current.endTimeSec)) {
            current = SubtitleCue();
            hasTiming = false;
            return;
        }
        if (current.endTimeSec <= current.startTimeSec) current.endTimeSec = current.startTimeSec + 2.5;
        if (totalCuesAdded < kMaxSubtitleCueCount) { outCues.push_back(std::move(current)); ++totalCuesAdded; }
        current = SubtitleCue();
        hasTiming = false;
    };
    while (std::getline(stream, line)) {
        if (totalCuesAdded >= kMaxSubtitleCueCount) break;
        if (line.size() > kMaxCueTextBytes) line.resize(kMaxCueTextBytes);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = TrimAscii(line);
        if (trimmed.empty()) { flush(); skipBlock = false; continue; }
        if (StartsWithInsensitive(trimmed, "WEBVTT") || StartsWithInsensitive(trimmed, "NOTE") ||
            StartsWithInsensitive(trimmed, "STYLE") || StartsWithInsensitive(trimmed, "REGION")) {
            flush();
            skipBlock = true;
            continue;
        }
        if (skipBlock) continue;
        double start = 0.0, end = 0.0;
        if (trimmed.find("-->") != std::string::npos && ParseTimingLine(trimmed, start, end, &SubtitleEngine::ParseTimestamp)) {
            if (hasTiming) flush();
            current.startTimeSec = std::max(0.0, start);
            current.endTimeSec = std::max(0.0, end);
            hasTiming = true;
            continue;
        }
        if (hasTiming) {
            std::wstring clean = CleanSubtitleFormatting(line);
            if (!clean.empty()) {
                if (current.text.size() + clean.size() + 1 <= kMaxCueTextBytes) {
                    if (!current.text.empty()) current.text += L'\n';
                    current.text += clean;
                }
            }
        }
    }
    flush();
    SortCues(outCues);
    return !outCues.empty();
}

bool SubtitleEngine::ParseVtt(const std::string& content, std::vector<SubtitleCue>& outCues) {
    if (content.empty() || content.size() > kMaxSubtitleInputBytes) { outCues.clear(); return false; }
    return ParseSrt(content, outCues);
}

bool SubtitleEngine::ParseAss(const std::string& content, std::vector<SubtitleCue>& outCues) {
    outCues.clear();
    if (content.empty() || content.size() > kMaxSubtitleInputBytes) return false;
    std::istringstream stream(content);
    std::string line;
    bool inEvents = false;
    std::vector<std::string> fields = { "layer", "start", "end", "style", "name", "marginl", "marginr", "marginv", "effect", "text" };
    size_t totalCuesAdded = 0;
    while (std::getline(stream, line)) {
        if (totalCuesAdded >= kMaxSubtitleCueCount) break;
        if (line.size() > kMaxCueTextBytes * 2) line.resize(kMaxCueTextBytes * 2);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = TrimAscii(line);
        if (trimmed.empty() || trimmed[0] == ';') continue;
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            std::string section = trimmed.substr(1, trimmed.size() - 2);
            for (char& c : section) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            inEvents = (section == "events" || section == "events]");
            continue;
        }
        if (!inEvents) continue;
        if (StartsWithInsensitive(trimmed, "format:")) {
            fields.clear();
            std::string format = TrimAscii(trimmed.substr(7));
            size_t begin = 0;
            while (begin <= format.size() && fields.size() < 64) {
                size_t comma = format.find(',', begin);
                std::string field = TrimAscii(format.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin));
                for (char& c : field) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                fields.push_back(field);
                if (comma == std::string::npos) break;
                begin = comma + 1;
            }
            continue;
        }
        if (!StartsWithInsensitive(trimmed, "dialogue:")) continue;
        std::string valuesText = TrimAscii(trimmed.substr(9));
        size_t textIndex = fields.empty() ? 9 : fields.size() - 1;
        for (size_t i = 0; i < fields.size(); ++i) {
            if (fields[i] == "text") { textIndex = i; break; }
        }
        std::vector<std::string> values;
        values.reserve(std::min(fields.size() + 1, static_cast<size_t>(64)));
        size_t begin = 0;
        for (size_t i = 0; i < textIndex && values.size() < 64; ++i) {
            size_t comma = valuesText.find(',', begin);
            if (comma == std::string::npos) { values.push_back(valuesText.substr(begin)); begin = valuesText.size(); break; }
            values.push_back(valuesText.substr(begin, comma - begin));
            begin = comma + 1;
        }
        if (values.size() < 64) { values.push_back(valuesText.substr(begin)); }
        std::string startText, endText, text;
        for (size_t i = 0; i < fields.size() && i < values.size(); ++i) {
            if (fields[i] == "start") startText = values[i];
            else if (fields[i] == "end") endText = values[i];
            else if (fields[i] == "text") text = values[i];
        }
        if (text.empty() && textIndex < values.size()) text = values[textIndex];
        SubtitleCue cue;
        cue.startTimeSec = std::max(0.0, ParseTimestamp(startText));
        cue.endTimeSec = std::max(0.0, ParseTimestamp(endText));
        cue.text = CleanSubtitleFormatting(text);
        if (!cue.text.empty() && std::isfinite(cue.startTimeSec) && std::isfinite(cue.endTimeSec)) {
            if (cue.endTimeSec <= cue.startTimeSec) cue.endTimeSec = cue.startTimeSec + 2.5;
            outCues.push_back(std::move(cue));
            ++totalCuesAdded;
        }
    }
    SortCues(outCues);
    return !outCues.empty();
}

bool SubtitleEngine::LoadSubtitleFile(const std::wstring& filePath, const std::wstring& trackName) {
    std::wstring normPath = SubtitleDiscovery::NormalizePath(filePath);
    if (normPath.empty() || SubtitleDiscovery::IsReservedDeviceName(normPath)) return false;
    std::ifstream file(normPath, std::ios::binary);
    if (!file.is_open()) return false;
    file.seekg(0, std::ios::end);
    std::streampos fileSize = file.tellg();
    if (fileSize < 0 || static_cast<size_t>(fileSize) > kMaxSubtitleInputBytes) { file.close(); return false; }
    file.seekg(0, std::ios::beg);
    std::string bytes(static_cast<size_t>(fileSize), '\0');
    if (fileSize > 0) {
        if (!file.read(&bytes[0], fileSize)) { file.close(); return false; }
    }
    file.close();
    std::string content = SubtitleDiscovery::DecodeSubtitleBytes(bytes);
    if (content.empty() || content.size() > kMaxSubtitleInputBytes) return false;
    std::wstring ext = GetFileExtension(normPath);
    std::vector<SubtitleCue> cues;
    bool success = false;
    if (ext == L".ass" || ext == L".ssa") success = ParseAss(content, cues);
    else if (ext == L".vtt") success = ParseVtt(content, cues);
    else success = ParseSrt(content, cues);
    if (!success || cues.empty()) return false;
    SubtitleTrack track;
    track.id = "sub_" + std::to_string(tracks.size() + 1);
    track.filePath = normPath;
    track.name = trackName.empty() ? GetFileNameFromPath(normPath) : trackName;
    track.cues = std::move(cues);
    track.isEmbedded = false;
    tracks.push_back(std::move(track));
    ++tracksGeneration;
    if (primaryTrackIndex == -1) { primaryTrackIndex = static_cast<int>(tracks.size()) - 1; activeTrackIndex = primaryTrackIndex; }
    return true;
}

void SubtitleEngine::LoadSubtitlesForMedia(const std::wstring& mediaPath) {
    Clear();
    std::vector<DiscoveredSubtitle> discovered = SubtitleDiscovery::DiscoverSubtitles(mediaPath);
    for (const auto& sub : discovered) { LoadSubtitleFile(sub.filePath, sub.title); }
}

std::wstring SubtitleEngine::GetCurrentSubtitleText(double currentPlaybackTimeSec, int delayMs) const {
    // The cache slot is keyed on the clamped delay, so non-zero delays get the
    // same cue-boundary caching instead of per-frame re-shaping work.
    if (!primaryVisible) return L"";
    return GetTrackTextCached(GetPrimaryTrack(), currentPlaybackTimeSec, delayMs, tracksGeneration, primaryTextCache);
}

std::wstring SubtitleEngine::GetPrimarySubtitleText(double currentPlaybackTimeSec) const {
    if (!primaryVisible) return L"";
    return GetTrackTextCached(GetPrimaryTrack(), currentPlaybackTimeSec, primaryDelayMs, tracksGeneration, primaryTextCache);
}

std::wstring SubtitleEngine::GetSecondarySubtitleText(double currentPlaybackTimeSec) const {
    if (!secondaryVisible) return L"";
    return GetTrackTextCached(GetSecondaryTrack(), currentPlaybackTimeSec, secondaryDelayMs, tracksGeneration, secondaryTextCache);
}

void SubtitleEngine::GetDualSubtitleText(double currentPlaybackTimeSec, std::wstring& outPrimary, std::wstring& outSecondary) const {
    outPrimary = GetPrimarySubtitleText(currentPlaybackTimeSec);
    outSecondary = GetSecondarySubtitleText(currentPlaybackTimeSec);
}

SubtitlePreferences SubtitleEngine::ExportPreferences(const std::wstring& rootDir) const {
    SubtitlePreferences prefs;
    prefs.primaryTrackIndex = primaryTrackIndex;
    prefs.secondaryTrackIndex = secondaryTrackIndex;
    if (const auto* p = GetPrimaryTrack())
        prefs.primaryTrackPath = rootDir.empty() ? p->filePath : SubtitleDiscovery::ToRootRelativePath(p->filePath, rootDir);
    if (const auto* s = GetSecondaryTrack())
        prefs.secondaryTrackPath = rootDir.empty() ? s->filePath : SubtitleDiscovery::ToRootRelativePath(s->filePath, rootDir);
    prefs.primaryDelayMs = primaryDelayMs;
    prefs.secondaryDelayMs = secondaryDelayMs;
    prefs.primaryVisible = primaryVisible;
    prefs.secondaryVisible = secondaryVisible;
    prefs.primaryPosition = primaryPosition;
    prefs.secondaryPosition = secondaryPosition;
    prefs.primaryFontSize = primaryFontSize;
    prefs.secondaryFontSize = secondaryFontSize;
    prefs.primaryColor = primaryColor;
    prefs.secondaryColor = secondaryColor;
    return prefs;
}

void SubtitleEngine::ImportPreferences(const SubtitlePreferences& prefs, const std::wstring& rootDir) {
    primaryDelayMs = SubtitleDiscovery::ClampDelayMs(prefs.primaryDelayMs);
    secondaryDelayMs = SubtitleDiscovery::ClampDelayMs(prefs.secondaryDelayMs);
    primaryVisible = prefs.primaryVisible;
    secondaryVisible = prefs.secondaryVisible;
    if (!prefs.primaryPosition.empty()) primaryPosition = prefs.primaryPosition;
    if (!prefs.secondaryPosition.empty()) secondaryPosition = prefs.secondaryPosition;
    if (!prefs.primaryFontSize.empty()) primaryFontSize = prefs.primaryFontSize;
    if (!prefs.secondaryFontSize.empty()) secondaryFontSize = prefs.secondaryFontSize;
    if (!prefs.primaryColor.empty()) primaryColor = prefs.primaryColor;
    if (!prefs.secondaryColor.empty()) secondaryColor = prefs.secondaryColor;
    if (prefs.primaryTrackIndex >= -1 && prefs.primaryTrackIndex < static_cast<int>(tracks.size()))
        SelectPrimaryTrack(prefs.primaryTrackIndex);
    if (prefs.secondaryTrackIndex >= -1 && prefs.secondaryTrackIndex < static_cast<int>(tracks.size()))
        SelectSecondaryTrack(prefs.secondaryTrackIndex);
    // If path was saved and track index is not found, try to match by filePath
    if (!prefs.primaryTrackPath.empty() && primaryTrackIndex == -1) {
        std::wstring fullPath = rootDir.empty() ? prefs.primaryTrackPath : SubtitleDiscovery::FromRootRelativePath(prefs.primaryTrackPath, rootDir);
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].filePath == fullPath) { SelectPrimaryTrack(static_cast<int>(i)); break; }
        }
    }
    if (!prefs.secondaryTrackPath.empty() && secondaryTrackIndex == -1) {
        std::wstring fullPath = rootDir.empty() ? prefs.secondaryTrackPath : SubtitleDiscovery::FromRootRelativePath(prefs.secondaryTrackPath, rootDir);
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].filePath == fullPath) { SelectSecondaryTrack(static_cast<int>(i)); break; }
        }
    }
}

} // namespace Aperture
