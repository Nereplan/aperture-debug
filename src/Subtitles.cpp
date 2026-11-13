#include "Subtitles.h"
#include <fstream>
#include <sstream>
#include <set>
#include <string_view>
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

std::string ToLowerAsciiCopy(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

// Removes <style>/<head> blocks (case-insensitive) so their contents never
// become SAMI cue text.
void EraseHtmlBlocks(std::string& content) {
    const std::string lower = ToLowerAsciiCopy(content);
    struct CharRange { size_t begin, end; };
    std::vector<CharRange> ranges;
    for (const char* tag : { "style", "head" }) {
        const std::string open = std::string("<") + tag;
        const std::string close = std::string("</") + tag;
        size_t searchPos = 0;
        while (searchPos < lower.size()) {
            size_t openPos = lower.find(open, searchPos);
            if (openPos == std::string::npos) break;
            size_t openEnd = lower.find('>', openPos);
            if (openEnd == std::string::npos) break;
            size_t closePos = lower.find(close, openEnd + 1);
            if (closePos == std::string::npos) break;
            size_t closeEnd = lower.find('>', closePos);
            if (closeEnd == std::string::npos) break;
            ranges.push_back({ openPos, closeEnd + 1 });
            searchPos = closeEnd + 1;
        }
    }
    if (ranges.empty()) return;
    // Merge overlapping ranges first (e.g. <STYLE> nested inside <HEAD>):
    // erasing a nested range shifts the offsets of the range that encloses it.
    std::sort(ranges.begin(), ranges.end(), [](const CharRange& a, const CharRange& b) { return a.begin < b.begin; });
    std::vector<CharRange> merged;
    for (const auto& range : ranges) {
        if (!merged.empty() && range.begin <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, range.end);
        } else {
            merged.push_back(range);
        }
    }
    for (auto it = merged.rbegin(); it != merged.rend(); ++it) content.erase(it->begin, it->end - it->begin);
}

// Parses the case-insensitive Start attribute of a <SYNC Start=1234> tag
// (optional quotes, milliseconds). Returns -1.0 when absent or invalid.
double ParseSamiSyncStartMs(const std::string& tag, const std::string& lowerTag) {
    size_t attrPos = 0;
    while ((attrPos = lowerTag.find("start", attrPos)) != std::string::npos) {
        size_t cursor = attrPos + 5;
        while (cursor < lowerTag.size() && (lowerTag[cursor] == ' ' || lowerTag[cursor] == '\t')) ++cursor;
        if (cursor < lowerTag.size() && lowerTag[cursor] == '=') {
            ++cursor;
            while (cursor < tag.size() && (tag[cursor] == ' ' || tag[cursor] == '\t')) ++cursor;
            if (cursor < tag.size() && (tag[cursor] == '"' || tag[cursor] == '\'')) ++cursor;
            char* endPtr = nullptr;
            const double ms = std::strtod(tag.c_str() + cursor, &endPtr);
            if (endPtr != tag.c_str() + cursor && std::isfinite(ms) && ms >= 0.0) return ms;
        }
        attrPos += 5;
    }
    return -1.0;
}

// MicroDVD .sub files begin with a {startFrame}{endFrame} line; other .sub
// content falls back to the SRT parser.
bool LooksLikeMicroDvd(const std::string& content) {
    size_t begin = 0;
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF) {
        begin = 3; // UTF-8 BOM
    }
    while (begin < content.size() && std::isspace(static_cast<unsigned char>(content[begin]))) ++begin;
    if (begin >= content.size() || content[begin] != '{') return false;
    size_t pos = begin + 1;
    auto skipDigits = [&content](size_t& p) {
        size_t start = p;
        while (p < content.size() && std::isdigit(static_cast<unsigned char>(content[p]))) ++p;
        return p > start;
    };
    if (!skipDigits(pos)) return false;
    if (pos >= content.size() || content[pos] != '}') return false;
    ++pos;
    if (pos < content.size() && content[pos] == '{') ++pos; // The end-frame field opens with '{'
    if (!skipDigits(pos)) return false;
    return pos < content.size() && content[pos] == '}';
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

constexpr size_t kCueBlockSize = 32;

void BuildCueEndIndex(const SubtitleTrack& track, SubtitleEngine::RenderedTextCacheSlot& slot) {
    slot.cueEndTree.clear();
    slot.cueBlockBase = 0;
    if (track.cues.size() > 64) {
        const size_t blocks = (track.cues.size() + kCueBlockSize - 1) / kCueBlockSize;
        size_t base = 1;
        while (base < blocks) base *= 2;
        slot.cueEndTree.assign(base * 2, -std::numeric_limits<double>::infinity());
        for (size_t i = 0; i < track.cues.size(); ++i) {
            const auto& cue = track.cues[i];
            if (cue.text.empty()) continue;
            // Parsed cues have finite ends. Keep manually supplied NaN ends
            // conservative, matching the rendering predicate below.
            const double end = std::isnan(cue.endTimeSec) ? std::numeric_limits<double>::infinity() : cue.endTimeSec;
            auto& maximum = slot.cueEndTree[base + i / kCueBlockSize];
            maximum = std::max(maximum, end);
        }
        for (size_t i = base - 1; i > 0; --i) {
            slot.cueEndTree[i] = std::max(slot.cueEndTree[i * 2], slot.cueEndTree[i * 2 + 1]);
        }
        slot.cueBlockBase = base;
    }
    slot.indexedCues = track.cues.data();
    slot.indexedCueCount = track.cues.size();
}

std::wstring GetTrackTextInternal(const SubtitleTrack* track, double currentPlaybackTimeSec, int delayMs,
                                 const SubtitleEngine::RenderedTextCacheSlot& slot, double* validUntilSec);

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
    if (slot.track != track || slot.generation != generation || slot.indexedCues != track->cues.data() ||
        slot.indexedCueCount != track->cues.size()) {
        BuildCueEndIndex(*track, slot);
        slot.validUntilSec = 0.0;
    }
    if (std::isfinite(adjustedTime) && adjustedTime >= 0.0 && slot.track == track && slot.delayMs == clampedDelayMs &&
        slot.generation == generation && adjustedTime >= slot.validFromSec && adjustedTime < slot.validUntilSec) {
        return slot.text;
    }
    double validUntil = 0.0;
    std::wstring rendered = GetTrackTextInternal(track, currentPlaybackTimeSec, clampedDelayMs, slot, &validUntil);
    slot.track = track;
    slot.delayMs = clampedDelayMs;
    slot.generation = generation;
    slot.validFromSec = std::isfinite(adjustedTime) && adjustedTime >= 0.0 ? adjustedTime : 0.0;
    slot.validUntilSec = validUntil;
    slot.text = rendered;
    return rendered;
}

std::wstring GetTrackTextInternal(const SubtitleTrack* track, double currentPlaybackTimeSec, int delayMs,
                                 const SubtitleEngine::RenderedTextCacheSlot& slot, double* validUntilSec) {
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
    std::set<std::wstring_view> seen;
    size_t renderedCount = 0;
    const size_t upperIndex = static_cast<size_t>(upper - track->cues.begin());
    auto renderRange = [&](size_t first, size_t end) {
        for (size_t i = first; i < end; ++i) {
            const auto& cue = track->cues[i];
            if (adjustedTime < cue.startTimeSec || adjustedTime > cue.endTimeSec || cue.text.empty()) continue;
            nextBoundary = std::min(nextBoundary, cue.endTimeSec);
            if (!seen.insert(cue.text).second) continue;
            if (!rendered.empty()) rendered += L'\n';
            rendered += cue.text;
            ++renderedCount;
            if (renderedCount >= 64 || rendered.size() >= SubtitleEngine::kMaxCueTextBytes) return false;
        }
        return true;
    };
    if (slot.cueBlockBase == 0) {
        renderRange(0, upperIndex);
    } else {
        // Visit in cue order, pruning expired subtrees even when an early,
        // long-running cue overlaps thousands of shorter, expired cues.
        auto visit = [&](auto&& self, size_t node, size_t first, size_t end) -> bool {
            if (first >= upperIndex || slot.cueEndTree[node] < adjustedTime) return true;
            if (node >= slot.cueBlockBase) return renderRange(first, std::min(end, upperIndex));
            const size_t middle = first + (end - first) / 2;
            return self(self, node * 2, first, middle) && self(self, node * 2 + 1, middle, end);
        };
        visit(visit, 1, 0, slot.cueBlockBase * kCueBlockSize);
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
    primaryTextCache = {};
    secondaryTextCache = {};
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

// Fallback frame rate for MicroDVD frame-based timing (files rarely carry a
// usable frame rate once the frame-index lines are read as plain integers).
constexpr double kMicroDvdFramesPerSecond = 25.0;

bool SubtitleEngine::ParseSami(const std::string& content, std::vector<SubtitleCue>& outCues) {
    outCues.clear();
    if (content.empty() || content.size() > kMaxSubtitleInputBytes) return false;
    std::string work = content;
    EraseHtmlBlocks(work);
    const std::string lower = ToLowerAsciiCopy(work);
    struct SamiSync { double startSec; size_t textBegin, textEnd; };
    std::vector<SamiSync> syncs;
    size_t searchPos = 0;
    while (searchPos < lower.size() && syncs.size() < kMaxSubtitleCueCount + 1) {
        size_t tagPos = lower.find("<sync", searchPos);
        if (tagPos == std::string::npos) break;
        size_t tagEnd = lower.find('>', tagPos);
        if (tagEnd == std::string::npos) break;
        size_t nextTagPos = lower.find("<sync", tagEnd + 1);
        const size_t textEnd = (nextTagPos != std::string::npos) ? nextTagPos : work.size();
        const double startMs = ParseSamiSyncStartMs(work.substr(tagPos, tagEnd - tagPos + 1), lower.substr(tagPos, tagEnd - tagPos + 1));
        if (startMs >= 0.0) { syncs.push_back({ startMs / 1000.0, tagEnd + 1, textEnd }); }
        searchPos = tagEnd + 1;
    }
    size_t totalCuesAdded = 0;
    for (size_t i = 0; i < syncs.size() && totalCuesAdded < kMaxSubtitleCueCount; ++i) {
        double endSec = -1.0;
        if (i + 1 < syncs.size()) endSec = syncs[i + 1].startSec; // End at the next SYNC start
        if (endSec <= syncs[i].startSec) endSec = syncs[i].startSec + 2.5; // Zero-duration rule / last-cue pad
        const size_t rawLength = std::min(syncs[i].textEnd - syncs[i].textBegin, kMaxCueTextBytes + 1);
        std::wstring text = CleanSubtitleFormatting(work.substr(syncs[i].textBegin, rawLength));
        if (text.empty()) continue;
        SubtitleCue cue;
        cue.startTimeSec = syncs[i].startSec;
        cue.endTimeSec = std::max(0.0, endSec);
        cue.text = std::move(text);
        outCues.push_back(std::move(cue));
        ++totalCuesAdded;
    }
    SortCues(outCues);
    return !outCues.empty();
}

bool SubtitleEngine::ParseMicroDvd(const std::string& content, std::vector<SubtitleCue>& outCues) {
    outCues.clear();
    if (content.empty() || content.size() > kMaxSubtitleInputBytes) return false;
    std::istringstream stream(content);
    std::string line;
    size_t totalCuesAdded = 0;
    while (std::getline(stream, line)) {
        if (totalCuesAdded >= kMaxSubtitleCueCount) break;
        if (line.size() > kMaxCueTextBytes * 2) line.resize(kMaxCueTextBytes * 2);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = TrimAscii(line);
        if (trimmed.empty() || trimmed.front() != '{') continue;
        char* endPtr = nullptr;
        const long long startFrame = std::strtoll(trimmed.c_str() + 1, &endPtr, 10);
        if (endPtr == trimmed.c_str() + 1 || *endPtr != '}' || startFrame < 0) continue;
        const char* secondBegin = endPtr + 1;
        if (*secondBegin == '{') ++secondBegin; // The end frame field opens with '{'
        const long long endFrame = std::strtoll(secondBegin, &endPtr, 10);
        if (endPtr == secondBegin || *endPtr != '}' || endFrame < startFrame) continue;
        std::string raw = trimmed.substr(static_cast<size_t>(endPtr - trimmed.c_str()) + 1);
        std::string text;
        text.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '|') { text.push_back('\n'); ++i; } // Escaped pipe separator
            else if (raw[i] == '|') { text.push_back('\n'); } // Pipe separator
            else { text.push_back(raw[i]); }
        }
        std::wstring clean = CleanSubtitleFormatting(text);
        if (clean.empty()) continue;
        SubtitleCue cue;
        cue.startTimeSec = static_cast<double>(startFrame) / kMicroDvdFramesPerSecond;
        cue.endTimeSec = static_cast<double>(endFrame) / kMicroDvdFramesPerSecond;
        if (cue.endTimeSec <= cue.startTimeSec) cue.endTimeSec = cue.startTimeSec + 2.5;
        cue.text = std::move(clean);
        outCues.push_back(std::move(cue));
        ++totalCuesAdded;
    }
    SortCues(outCues);
    return !outCues.empty();
}

bool SubtitleEngine::ParseVPlayer(const std::string& content, std::vector<SubtitleCue>& outCues) {
    outCues.clear();
    if (content.empty() || content.size() > kMaxSubtitleInputBytes) return false;
    std::istringstream stream(content);
    std::string line;
    size_t totalCuesAdded = 0;
    while (std::getline(stream, line)) {
        if (totalCuesAdded >= kMaxSubtitleCueCount) break;
        if (line.size() > kMaxCueTextBytes * 2) line.resize(kMaxCueTextBytes * 2);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = TrimAscii(line);
        if (trimmed.empty()) continue;
        // Timestamp token runs to the first space (H:MM:SS.ss or MM:SS.ss with
        // an optional leading zero); text starts after the whitespace run.
        const size_t timestampEnd = trimmed.find_first_of(" \t");
        if (timestampEnd == std::string::npos) continue;
        const double startSec = ParseTimestamp(trimmed.substr(0, timestampEnd));
        if (!std::isfinite(startSec) || startSec < 0.0) continue;
        const size_t textBegin = trimmed.find_first_not_of(" \t", timestampEnd);
        if (textBegin == std::string::npos) continue;
        std::wstring clean = CleanSubtitleFormatting(trimmed.substr(textBegin));
        if (clean.empty()) continue;
        SubtitleCue cue;
        cue.startTimeSec = startSec;
        cue.endTimeSec = startSec + 2.5;
        cue.text = std::move(clean);
        outCues.push_back(std::move(cue));
        ++totalCuesAdded;
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
    else if (ext == L".smi" || ext == L".sami") success = ParseSami(content, cues);
    else if (ext == L".sub") success = LooksLikeMicroDvd(content) ? ParseMicroDvd(content, cues) : ParseSrt(content, cues);
    else if (ext == L".pjs" || ext == L".vplayer") success = ParseVPlayer(content, cues);
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
    std::vector<DiscoveredSubtitle> discovered = SubtitleDiscovery::DiscoverSubtitles(mediaPath, false);
    for (const auto& sub : discovered) { LoadSubtitleFile(sub.filePath, sub.title); }
}

DWORD SubtitleEngine::GetNextTransitionWaitMs(double currentPlaybackTimeSec, int primaryOverrideDelayMs,
                                              double playbackRate, DWORD maxWaitMs) const {
    if (maxWaitMs <= 1) return maxWaitMs;
    if (!std::isfinite(currentPlaybackTimeSec)) return maxWaitMs;
    const double safeRate = (std::isfinite(playbackRate) && playbackRate > 0.0)
        ? std::clamp(playbackRate, 0.25, 4.0) : 1.0;
    auto nextForTrack = [&](const SubtitleTrack* track, RenderedTextCacheSlot& slot, int delayMs) -> DWORD {
        if (!track || track->cues.empty()) return maxWaitMs;
        const int clampedDelayMs = SubtitleDiscovery::ClampDelayMs(delayMs);
        const double adjustedTime = currentPlaybackTimeSec - static_cast<double>(clampedDelayMs) / 1000.0;
        if (!std::isfinite(adjustedTime)) return maxWaitMs;
        double nextBoundary = std::numeric_limits<double>::infinity();
        if (adjustedTime < 0.0) {
            for (const auto& cue : track->cues) {
                if (!cue.text.empty() && std::isfinite(cue.startTimeSec)) {
                    nextBoundary = std::max(0.0, cue.startTimeSec);
                    break;
                }
            }
        } else {
            GetTrackTextCached(track, currentPlaybackTimeSec, clampedDelayMs, tracksGeneration, slot);
            nextBoundary = slot.validUntilSec;
        }
        if (!std::isfinite(nextBoundary)) return maxWaitMs;
        const double deltaMediaSec = nextBoundary - adjustedTime;
        if (deltaMediaSec <= 0.0) return 1;
        const double realMs = (deltaMediaSec / safeRate) * 1000.0;
        if (realMs >= static_cast<double>(maxWaitMs)) return maxWaitMs;
        return static_cast<DWORD>(std::clamp(std::ceil(realMs), 1.0, static_cast<double>(maxWaitMs)));
    };
    DWORD waitMs = maxWaitMs;
    if (primaryVisible) {
        waitMs = std::min(waitMs, nextForTrack(GetPrimaryTrack(), primaryTextCache, primaryOverrideDelayMs));
    }
    if (secondaryVisible) {
        waitMs = std::min(waitMs, nextForTrack(GetSecondaryTrack(), secondaryTextCache, secondaryDelayMs));
    }
    return waitMs;
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
