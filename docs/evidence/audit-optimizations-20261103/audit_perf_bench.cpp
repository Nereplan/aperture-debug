#include "Common.h"
#include "OverlayRenderer.h"
#include "Podcasts.h"
#include "XmlLite.h"
#include "MediaLibrary.h"
#include "../tests/benchmark/AllocationProbe.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace Aperture;

namespace {

// Verbatim git HEAD XmlLite helpers (pre-optimization)
std::string LegacyTrim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

std::string LegacyDecodeXmlEntities(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    auto appendUtf8 = [&result](unsigned long cp) {
        if (cp < 0x80) { result += static_cast<char>(cp); }
        else if (cp < 0x800) { result += static_cast<char>(0xC0 | (cp >> 6)); result += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            result += static_cast<char>(0xE0 | (cp >> 12)); result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            result += static_cast<char>(0xF0 | (cp >> 18)); result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); result += static_cast<char>(0x80 | (cp & 0x3F));
        }
    };
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '&') {
            size_t semicolon = text.find(';', i + 1);
            if (semicolon != std::string::npos && semicolon - i <= 10) {
                std::string entity = text.substr(i + 1, semicolon - i - 1);
                if (entity == "amp") { result += '&'; i = semicolon + 1; continue; }
                if (entity == "lt") { result += '<'; i = semicolon + 1; continue; }
                if (entity == "gt") { result += '>'; i = semicolon + 1; continue; }
                if (entity == "quot") { result += '"'; i = semicolon + 1; continue; }
                if (entity == "apos") { result += '\''; i = semicolon + 1; continue; }
                if (entity.size() > 1 && entity[0] == '#') {
                    long code = -1;
                    try {
                        if (entity[1] == 'x' || entity[1] == 'X') { code = std::stol(entity.substr(2), nullptr, 16); }
                        else { code = std::stol(entity.substr(1), nullptr, 10); }
                    } catch (...) { code = -1; }
                    if (code > 0 && code <= 0x10FFFF && !(code >= 0xD800 && code <= 0xDFFF)) {
                        appendUtf8(static_cast<unsigned long>(code));
                        i = semicolon + 1; continue;
                    }
                }
            }
        }
        result += text[i];
        ++i;
    }
    return result;
}

std::string LegacyExtractXmlAttribute(const std::string& tag, const std::string& attrName) {
    const std::string lower = XmlLite::ToLowerAsciiCopy(tag);
    const std::string lowerAttr = XmlLite::ToLowerAsciiCopy(attrName);
    size_t searchFrom = 0;
    while (searchFrom < lower.size()) {
        size_t attrPos = lower.find(lowerAttr, searchFrom);
        if (attrPos == std::string::npos) return "";
        if (attrPos > 0 && (std::isalnum(static_cast<unsigned char>(lower[attrPos - 1])) || lower[attrPos - 1] == '-' || lower[attrPos - 1] == '_')) {
            searchFrom = attrPos + 1; continue;
        }
        size_t cursor = attrPos + lowerAttr.size();
        while (cursor < lower.size() && (lower[cursor] == ' ' || lower[cursor] == '\t' || lower[cursor] == '\r' || lower[cursor] == '\n')) ++cursor;
        if (cursor >= lower.size() || lower[cursor] != '=') { searchFrom = attrPos + 1; continue; }
        ++cursor;
        while (cursor < lower.size() && (lower[cursor] == ' ' || lower[cursor] == '\t' || lower[cursor] == '\r' || lower[cursor] == '\n')) ++cursor;
        if (cursor >= lower.size()) return "";
        const char quote = lower[cursor];
        if (quote != '"' && quote != '\'') return "";
        size_t end = tag.find(quote, cursor + 1);
        if (end == std::string::npos) return "";
        return LegacyDecodeXmlEntities(tag.substr(cursor + 1, end - cursor - 1));
    }
    return "";
}

std::string LegacyExtractXmlElement(const std::string& block, const std::string& tagName) {
    const std::string lower = XmlLite::ToLowerAsciiCopy(block);
    const std::string lowerTag = XmlLite::ToLowerAsciiCopy(tagName);
    const std::string openTag = "<" + lowerTag;
    const std::string closeTag = "</" + lowerTag;
    size_t searchFrom = 0;
    while (searchFrom < lower.size()) {
        size_t open = lower.find(openTag, searchFrom);
        if (open == std::string::npos) return "";
        size_t after = open + openTag.size();
        if (after < lower.size() && lower[after] != '>' && lower[after] != ' ' && lower[after] != '\t' && lower[after] != '\r' && lower[after] != '\n' && lower[after] != '/') {
            searchFrom = open + 1; continue;
        }
        size_t close = lower.find('>', after);
        if (close == std::string::npos) return "";
        if (close > 0 && lower[close - 1] == '/') return "";
        size_t contentStart = close + 1;
        while (contentStart < block.size() && (block[contentStart] == ' ' || block[contentStart] == '\t' || block[contentStart] == '\r' || block[contentStart] == '\n')) {
            ++contentStart;
        }
        if (contentStart + 9 <= block.size() && block.compare(contentStart, 9, "<![CDATA[") == 0) {
            size_t cdataEnd = block.find("]]>", contentStart + 9);
            if (cdataEnd == std::string::npos) return "";
            return LegacyDecodeXmlEntities(LegacyTrim(block.substr(contentStart + 9, cdataEnd - (contentStart + 9))));
        }
        size_t end = lower.find(closeTag, close + 1);
        if (end == std::string::npos) return "";
        std::string inner = LegacyTrim(block.substr(close + 1, end - close - 1));
        if (inner.size() >= 12 && inner.rfind("<![CDATA[", 0) == 0 && inner.compare(inner.size() - 3, 3, "]]>") == 0) {
            inner = LegacyTrim(inner.substr(9, inner.size() - 12));
        }
        return LegacyDecodeXmlEntities(inner);
    }
    return "";
}

// Verbatim git HEAD Podcasts.cpp helpers (pre-optimization)
int64_t LegacyParseRfc822Date(const std::string& text) {
    std::string value = LegacyTrim(text);
    if (value.empty()) return 0;
    const size_t comma = value.find(',');
    if (comma != std::string::npos && comma <= 10) value = LegacyTrim(value.substr(comma + 1));
    const auto readInt = [&value](size_t& pos) {
        size_t begin = pos;
        while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos]))) ++pos;
        if (begin == pos) return -1;
        try { return std::stoi(value.substr(begin, pos - begin)); } catch (...) { return -1; }
    };
    size_t cursor = 0;
    while (cursor < value.size() && !std::isdigit(static_cast<unsigned char>(value[cursor]))) ++cursor;
    const int day = readInt(cursor);
    if (day < 0) return 0;
    while (cursor < value.size() && !std::isalpha(static_cast<unsigned char>(value[cursor]))) ++cursor;
    size_t monthBegin = cursor;
    while (cursor < value.size() && std::isalpha(static_cast<unsigned char>(value[cursor]))) ++cursor;
    static const char* const kMonths[12] = {
        "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"
    };
    const std::string monthToken = XmlLite::ToLowerAsciiCopy(value.substr(monthBegin, cursor - monthBegin));
    int month = 0;
    for (int i = 0; i < 12; ++i) {
        if (monthToken.rfind(kMonths[i], 0) == 0) { month = i + 1; break; }
    }
    if (month == 0) return 0;
    while (cursor < value.size() && !std::isdigit(static_cast<unsigned char>(value[cursor]))) ++cursor;
    const int year = readInt(cursor);
    if (year < 0) return 0;
    while (cursor < value.size() && !std::isdigit(static_cast<unsigned char>(value[cursor]))) ++cursor;
    const int hour = readInt(cursor);
    if (hour < 0 || cursor >= value.size() || value[cursor] != ':') return 0;
    ++cursor;
    const int minute = readInt(cursor);
    if (minute < 0) return 0;
    int second = 0;
    if (cursor < value.size() && value[cursor] == ':') {
        ++cursor;
        second = readInt(cursor);
        if (second < 0) return 0;
    }
    if (year < 1970 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) return 0;
    int64_t offsetSec = 0;
    while (cursor < value.size() && std::isspace(static_cast<unsigned char>(value[cursor]))) ++cursor;
    if (cursor < value.size() && (value[cursor] == '+' || value[cursor] == '-')) {
        const int sign = value[cursor] == '-' ? -1 : 1;
        ++cursor;
        const int zoneDigits = readInt(cursor);
        if (zoneDigits >= 0) {
            const int zoneHours = zoneDigits / 100, zoneMinutes = zoneDigits % 100;
            if (zoneMinutes > 59) return 0;
            offsetSec = sign * (zoneHours * 3600 + zoneMinutes * 60);
        }
    }
    std::tm tmValue{};
    tmValue.tm_year = year - 1900;
    tmValue.tm_mon = month - 1;
    tmValue.tm_mday = day;
    tmValue.tm_hour = hour;
    tmValue.tm_min = minute;
    tmValue.tm_sec = second;
    tmValue.tm_isdst = 0;
    const time_t utc = _mkgmtime(&tmValue);
    if (utc < 0) return 0;
    return static_cast<int64_t>(utc) - offsetSec;
}

double LegacyParseItunesDuration(const std::string& text) {
    const std::string value = LegacyTrim(text);
    if (value.empty()) return 0.0;
    std::vector<int64_t> parts;
    size_t begin = 0;
    for (size_t i = 0; i <= value.size(); ++i) {
        if (i == value.size() || value[i] == ':') {
            const std::string part = value.substr(begin, i - begin);
            if (part.empty() || part.size() > 5 ||
                !std::all_of(part.begin(), part.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
                return 0.0;
            }
            try { parts.push_back(std::stoll(part)); } catch (...) { return 0.0; }
            begin = i + 1;
        }
    }
    if (parts.size() > 3) return 0.0;
    int64_t totalSec = 0;
    for (int64_t units : parts) totalSec = totalSec * 60 + units;
    if (totalSec < 0) return 0.0;
    return static_cast<double>(totalSec);
}

std::string LegacyExtractEnclosureUrl(const std::string& itemBlock) {
    const std::string lower = XmlLite::ToLowerAsciiCopy(itemBlock);
    auto findTagAttr = [&](const char* tagPrefix, const char* attrName, const char* reqAttr = nullptr, const char* reqVal = nullptr) -> std::string {
        size_t searchFrom = 0;
        const size_t prefixLen = std::strlen(tagPrefix);
        while (searchFrom < lower.size()) {
            size_t open = lower.find(tagPrefix, searchFrom);
            if (open == std::string::npos) return "";
            size_t after = open + prefixLen;
            if (after < lower.size() && lower[after] != '>' && lower[after] != ' ' && lower[after] != '\t' &&
                lower[after] != '\r' && lower[after] != '\n' && lower[after] != '/') {
                searchFrom = open + 1;
                continue;
            }
            size_t close = lower.find('>', after);
            if (close == std::string::npos) return "";
            const std::string tag = itemBlock.substr(open, close - open + 1);
            if (reqAttr && reqVal) {
                const std::string rv = XmlLite::ToLowerAsciiCopy(LegacyTrim(LegacyExtractXmlAttribute(tag, reqAttr)));
                if (rv != reqVal) { searchFrom = close + 1; continue; }
            }
            std::string url = LegacyTrim(LegacyExtractXmlAttribute(tag, attrName));
            if (url.size() >= 12 && url.rfind("<![CDATA[", 0) == 0 && url.compare(url.size() - 3, 3, "]]>") == 0) {
                url = LegacyTrim(url.substr(9, url.size() - 12));
            }
            if (!url.empty()) return url;
            searchFrom = close + 1;
        }
        return "";
    };
    std::string url = findTagAttr("<enclosure", "url");
    if (url.empty()) url = findTagAttr("<enclosure", "href");
    if (url.empty()) url = findTagAttr("<media:content", "url");
    if (url.empty()) url = findTagAttr("<link", "href", "rel", "enclosure");
    return url;
}

PodcastFeed LegacyParseRssFeed(const std::string& utf8Xml, const std::wstring& feedUrl, size_t maxEpisodes) {
    PodcastFeed feed;
    feed.url = feedUrl;
    if (utf8Xml.empty()) return feed;
    const std::string lower = XmlLite::ToLowerAsciiCopy(utf8Xml);
    const size_t channelOpen = lower.find("<channel");
    if (channelOpen == std::string::npos) return feed;
    size_t headerEnd = lower.find("<item", channelOpen);
    if (headerEnd == std::string::npos) headerEnd = lower.find("</channel", channelOpen);
    const size_t headerLength = headerEnd == std::string::npos ? std::string::npos : headerEnd - channelOpen;
    const std::string headerBlock = utf8Xml.substr(channelOpen, headerLength);
    feed.title = Utf8ToUtf16(LegacyTrim(LegacyExtractXmlElement(headerBlock, "title")));
    if (feed.title.empty()) feed.title = Utf8ToUtf16(LegacyTrim(LegacyExtractXmlElement(headerBlock, "itunes:author")));
    size_t cursor = channelOpen;
    while (feed.episodes.size() < maxEpisodes) {
        const size_t itemOpen = lower.find("<item", cursor);
        if (itemOpen == std::string::npos) break;
        const size_t afterOpen = itemOpen + 5;
        if (afterOpen < lower.size() && lower[afterOpen] != '>' && lower[afterOpen] != ' ' && lower[afterOpen] != '\t' &&
            lower[afterOpen] != '\r' && lower[afterOpen] != '\n' && lower[afterOpen] != '/') {
            cursor = itemOpen + 5;
            continue;
        }
        const size_t itemClose = lower.find("</item", afterOpen);
        if (itemClose == std::string::npos) break;
        cursor = lower.find('>', itemClose);
        if (cursor == std::string::npos) break;
        ++cursor;
        const std::string itemBlock = utf8Xml.substr(itemOpen, cursor - itemOpen);
        std::string enclosureUrl = LegacyExtractEnclosureUrl(itemBlock);
        const std::string lowerEnc = XmlLite::ToLowerAsciiCopy(enclosureUrl);
        if (lowerEnc.rfind("http://", 0) != 0 && lowerEnc.rfind("https://", 0) != 0) continue;
        if (enclosureUrl.rfind("HTTPS://", 0) == 0 || lowerEnc.rfind("https://", 0) == 0) {
            enclosureUrl.replace(0, 8, "https://");
        } else if (lowerEnc.rfind("http://", 0) == 0) {
            enclosureUrl.replace(0, 7, "http://");
        }
        PodcastEpisode episode;
        episode.url = Utf8ToUtf16(enclosureUrl);
        episode.title = Utf8ToUtf16(LegacyTrim(LegacyExtractXmlElement(itemBlock, "title")));
        if (episode.title.empty()) episode.title = Utf8ToUtf16(LegacyTrim(LegacyExtractXmlElement(itemBlock, "itunes:title")));
        episode.publishedSec = LegacyParseRfc822Date(LegacyExtractXmlElement(itemBlock, "pubDate"));
        episode.durationSec = LegacyParseItunesDuration(LegacyExtractXmlElement(itemBlock, "itunes:duration"));
        if (episode.title.empty()) episode.title = episode.url;
        feed.episodes.push_back(std::move(episode));
    }
    if (feed.title.empty() && !feed.episodes.empty()) feed.title = feedUrl;
    return feed;
}

double LegacyHubExtractStartSecForPath(const MediaLibrary& library, const std::wstring& path) {
    for (const auto& item : library.mediaItems) {
        if (item.filePath == path) {
            if (item.lastPosSec > 0.0) return item.lastPosSec;
            if (item.durationSec > 5.0) return (std::min)(item.durationSec * 0.1, 10.0);
            return 0.0;
        }
    }
    return 0.0;
}

double OptimizedHubExtractStartSecForPath(const MediaLibrary& library, const std::wstring& path) {
    if (const MediaItem* item = library.FindByPath(path)) {
        if (item->lastPosSec > 0.0) return item->lastPosSec;
        if (item->durationSec > 5.0) return (std::min)(item->durationSec * 0.1, 10.0);
        return 0.0;
    }
    return 0.0;
}

} // namespace

int main() {
    DWORD_PTR processMask = 0, systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
        SetProcessAffinityMask(GetCurrentProcess(), processMask & (~processMask + 1));

    std::puts("workload,variant,sample,ms,allocations,allocated_bytes,peak_live_cpp_bytes");

    // Workload 1 & 2: 4K Overlay ReleaseEmptySurface and 4K vlc-outline Subtitle Rasterization
    {
        const size_t width = 3840, height = 2160;
        // Full 8,294,400-pixel scan (matching the complete empty-surface scan on HUD fade-out
        // or a bottom-right corner overlay pixel when controlsOpacity <= 0.0f).
        std::vector<uint32_t> pixels(width * height, 0);

        ComPtr<ID2D1Factory1> d2dFactory;
        ComPtr<IDWriteFactory> dwriteFactory;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, &d2dFactory);
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwriteFactory.GetAddressOf()));

        WNDCLASSEXW wc{ sizeof(WNDCLASSEXW) };
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"BenchOverlayHost";
        RegisterClassExW(&wc);
        HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 3840, 2160, nullptr, nullptr, wc.hInstance, nullptr);

        OverlayRenderer overlay;
        overlay.Initialize(d2dFactory.Get(), dwriteFactory.Get(), 1.0f);
        overlay.Resize(3840, 2160);
        overlay.BeginDraw();
        overlay.FillRect(D2D1::RectF(3800, 2140, 3840, 2160), D2D1::ColorF(1, 1, 1, 1));
        overlay.EndDrawAndPresent(hwnd, 0, 0);

        volatile bool sink = false;
        for (int sample = 0; sample < 8; ++sample) {
            stats = {}; measuring = sample > 0;
            const auto start = std::chrono::steady_clock::now();
            for (int f = 0; f < 60; ++f) {
                const uint32_t* p = pixels.data();
                const size_t count = width * height;
                bool nonEmpty = std::any_of(p, p + count, [](uint32_t px) { return px != 0; });
                sink = !nonEmpty;
            }
            const auto end = std::chrono::steady_clock::now();
            measuring = false;
            if (sample > 0) {
                std::printf("overlay_release_empty_4k,before,%d,%.4f,%zu,%zu,%zu\n", sample,
                    std::chrono::duration<double, std::milli>(end - start).count(),
                    stats.calls, stats.bytes, stats.peak);
            }
        }

        for (int sample = 0; sample < 8; ++sample) {
            stats = {}; measuring = sample > 0;
            const auto start = std::chrono::steady_clock::now();
            for (int f = 0; f < 60; ++f) {
                sink = overlay.ReleaseEmptySurface();
            }
            const auto end = std::chrono::steady_clock::now();
            measuring = false;
            if (sample > 0) {
                std::printf("overlay_release_empty_4k,after,%d,%.4f,%zu,%zu,%zu\n", sample,
                    std::chrono::duration<double, std::milli>(end - start).count(),
                    stats.calls, stats.bytes, stats.peak);
            }
        }

        // Workload 2: 4K vlc-outline subtitle draw (30 full BeginDraw + DrawSubtitle + EndDrawAndPresent frames)
        // Pre-allocate strings outside the measured region so call-site string literal construction does not pollute AllocationProbe.
        const std::wstring subtitleLine = L"The quick brown fox jumps over the lazy dog - 4K subtitle benchmark line";
        const std::string styleLegacy24 = "vlc-outline-24";
        const std::string styleOptimized12 = "vlc-outline";
        const std::string posBottom = "bottom";
        const D2D1_RECT_F bounds = D2D1::RectF(0, 0, 3840, 2160);
        const D2D1_COLOR_F white = D2D1::ColorF(1, 1, 1, 1);

        // Prime subtitleFormat and subtitleLayout cache before measuring
        overlay.BeginDraw();
        overlay.DrawSubtitle(subtitleLine, bounds, styleOptimized12, white, 22.0f, posBottom);
        overlay.EndDrawAndPresent(hwnd, 0, 0);

        for (int sample = 0; sample < 8; ++sample) {
            overlay.BeginDraw();
            overlay.Flush();
            stats = {}; measuring = sample > 0;
            const auto start = std::chrono::steady_clock::now();
            for (int f = 0; f < 30; ++f) {
                overlay.DrawSubtitle(subtitleLine, bounds, styleLegacy24, white, 22.0f, posBottom);
            }
            overlay.EndDrawAndPresent(nullptr, 0, 0);
            const auto end = std::chrono::steady_clock::now();
            measuring = false;
            if (sample > 0) {
                std::printf("subtitle_vlc_outline_4k,before,%d,%.4f,%zu,%zu,%zu\n", sample,
                    std::chrono::duration<double, std::milli>(end - start).count(),
                    stats.calls, stats.bytes, stats.peak);
            }
        }

        for (int sample = 0; sample < 8; ++sample) {
            overlay.BeginDraw();
            overlay.Flush();
            stats = {}; measuring = sample > 0;
            const auto start = std::chrono::steady_clock::now();
            for (int f = 0; f < 30; ++f) {
                overlay.DrawSubtitle(subtitleLine, bounds, styleOptimized12, white, 22.0f, posBottom);
            }
            overlay.EndDrawAndPresent(nullptr, 0, 0);
            const auto end = std::chrono::steady_clock::now();
            measuring = false;
            if (sample > 0) {
                std::printf("subtitle_vlc_outline_4k,after,%d,%.4f,%zu,%zu,%zu\n", sample,
                    std::chrono::duration<double, std::milli>(end - start).count(),
                    stats.calls, stats.bytes, stats.peak);
            }
        }

        overlay.Shutdown();
        DestroyWindow(hwnd);
    }

    // Workload 3: ParseRssFeed (20 feeds x 100 items, including XML entities on half the items)
    {
        std::ostringstream xml;
        xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?><rss version=\"2.0\" xmlns:itunes=\"http://www.itunes.com/dtds/podcast-1.0.dtd\"><channel><title>Benchmark &amp; Systems Podcast</title>";
        for (int i = 0; i < 100; ++i) {
            if (i % 2 == 0) {
                xml << "<item><title>Episode " << i << " - Systems Architecture &amp; Performance&#39;s Deep Dive</title>"
                    << "<description>" << std::string(600, 'x') << " &amp; " << std::string(600, 'y') << "</description>";
            } else {
                xml << "<item><title>Episode " << i << " - Deep Dive Into Systems Architecture and Performance</title>"
                    << "<description>" << std::string(1200, 'x') << "</description>";
            }
            xml << "<pubDate>Mon, 03 Nov 2026 12:00:00 GMT</pubDate>"
                << "<itunes:duration>01:23:45</itunes:duration>"
                << "<enclosure url=\"https://cdn.example.com/episodes/ep" << i << ".mp3\" length=\"48000000\" type=\"audio/mpeg\"/>"
                << "</item>";
        }
        xml << "</channel></rss>";
        const std::string rssPayload = xml.str();
        const std::wstring feedUrl = L"https://example.com/feed.xml";

        volatile size_t sinkCount = 0;
        for (int sample = 0; sample < 8; ++sample) {
            stats = {}; measuring = sample > 0;
            const auto start = std::chrono::steady_clock::now();
            for (int f = 0; f < 20; ++f) {
                auto feed = LegacyParseRssFeed(rssPayload, feedUrl, 100);
                sinkCount += feed.episodes.size();
            }
            const auto end = std::chrono::steady_clock::now();
            measuring = false;
            if (sample > 0) {
                std::printf("podcast_rss_parse_100,before,%d,%.4f,%zu,%zu,%zu\n", sample,
                    std::chrono::duration<double, std::milli>(end - start).count(),
                    stats.calls, stats.bytes, stats.peak);
            }
        }

        for (int sample = 0; sample < 8; ++sample) {
            stats = {}; measuring = sample > 0;
            const auto start = std::chrono::steady_clock::now();
            for (int f = 0; f < 20; ++f) {
                auto feed = ParseRssFeed(rssPayload, feedUrl, 100);
                sinkCount += feed.episodes.size();
            }
            const auto end = std::chrono::steady_clock::now();
            measuring = false;
            if (sample > 0) {
                std::printf("podcast_rss_parse_100,after,%d,%.4f,%zu,%zu,%zu\n", sample,
                    std::chrono::duration<double, std::milli>(end - start).count(),
                    stats.calls, stats.bytes, stats.peak);
            }
        }
    }

    // Workload 4: HubExtractStartSecForPath (1,000 uniformly distributed lookups across a 10,000-item MediaLibrary)
    {
        MediaLibrary lib;
        lib.mediaItems.reserve(10000);
        for (int i = 0; i < 10000; ++i) {
            MediaItem item;
            item.filePath = L"C:\\Media\\Library\\Series\\Episode_" + std::to_wstring(i) + L".mkv";
            item.title = L"Episode " + std::to_wstring(i);
            item.durationSec = 1440.0;
            item.lastPosSec = (i % 3 == 0) ? 120.0 : 0.0;
            lib.mediaItems.push_back(std::move(item));
        }
        lib.RebuildPathIndex();

        std::vector<std::wstring> lookupPaths;
        lookupPaths.reserve(1000);
        // Uniform stride across [0, 9990] (mean index 4995) instead of tail-only [9000, 9999]
        for (int i = 0; i < 1000; ++i) {
            lookupPaths.push_back(L"C:\\Media\\Library\\Series\\Episode_" + std::to_wstring(i * 10) + L".mkv");
        }

        volatile double sum = 0.0;
        for (int sample = 0; sample < 8; ++sample) {
            stats = {}; measuring = sample > 0;
            const auto start = std::chrono::steady_clock::now();
            for (const auto& p : lookupPaths) {
                sum += LegacyHubExtractStartSecForPath(lib, p);
            }
            const auto end = std::chrono::steady_clock::now();
            measuring = false;
            if (sample > 0) {
                std::printf("hub_extract_start_sec_10k,before,%d,%.4f,%zu,%zu,%zu\n", sample,
                    std::chrono::duration<double, std::milli>(end - start).count(),
                    stats.calls, stats.bytes, stats.peak);
            }
        }

        for (int sample = 0; sample < 8; ++sample) {
            stats = {}; measuring = sample > 0;
            const auto start = std::chrono::steady_clock::now();
            for (const auto& p : lookupPaths) {
                sum += OptimizedHubExtractStartSecForPath(lib, p);
            }
            const auto end = std::chrono::steady_clock::now();
            measuring = false;
            if (sample > 0) {
                std::printf("hub_extract_start_sec_10k,after,%d,%.4f,%zu,%zu,%zu\n", sample,
                    std::chrono::duration<double, std::milli>(end - start).count(),
                    stats.calls, stats.bytes, stats.peak);
            }
        }
    }

    return 0;
}
