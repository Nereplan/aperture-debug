#pragma once

// Shared minimal XML text helpers: entity decoding and case-insensitive
// element/attribute extraction for the lightweight XML dialects the player
// reads (XSPF/ASX/WPL playlists, RSS podcast feeds). Header-only so any
// translation unit can parse XML without a dedicated XML dependency.

#include <cctype>
#include <charconv>
#include <string>
#include <string_view>

namespace Aperture::XmlLite {

// Helper: Trim string view without allocating
inline std::string_view TrimView(std::string_view str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

// Helper: Trim string
inline std::string Trim(std::string_view str) {
    return std::string(TrimView(str));
}

inline std::string ToLowerAsciiCopy(std::string str) {
    for (auto& c : str) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return str;
}

inline std::string DecodeXmlEntities(std::string_view text) {
    size_t firstAmp = text.find('&');
    if (firstAmp == std::string_view::npos) return std::string(text);
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
    size_t i = 0;
    size_t amp = firstAmp;
    while (amp != std::string_view::npos) {
        if (amp > i) result.append(text.data() + i, amp - i);
        size_t semicolon = text.find(';', amp + 1);
        if (semicolon != std::string_view::npos && semicolon - amp <= 10) {
            std::string_view entity = text.substr(amp + 1, semicolon - amp - 1);
            if (entity == "amp") { result += '&'; i = semicolon + 1; amp = text.find('&', i); continue; }
            if (entity == "lt") { result += '<'; i = semicolon + 1; amp = text.find('&', i); continue; }
            if (entity == "gt") { result += '>'; i = semicolon + 1; amp = text.find('&', i); continue; }
            if (entity == "quot") { result += '"'; i = semicolon + 1; amp = text.find('&', i); continue; }
            if (entity == "apos") { result += '\''; i = semicolon + 1; amp = text.find('&', i); continue; }
            if (entity.size() > 1 && entity[0] == '#') {
                unsigned long code = 0;
                const bool isHex = (entity[1] == 'x' || entity[1] == 'X');
                std::string_view digits = entity.substr(isHex ? 2 : 1);
                if (!digits.empty()) {
                    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), code, isHex ? 16 : 10);
                    if (parsed.ec == std::errc() && parsed.ptr == digits.data() + digits.size() &&
                        code > 0 && code <= 0x10FFFF && !(code >= 0xD800 && code <= 0xDFFF)) {
                        appendUtf8(code);
                        i = semicolon + 1;
                        amp = text.find('&', i);
                        continue;
                    }
                }
            }
        }
        result += '&';
        i = amp + 1;
        amp = text.find('&', i);
    }
    if (i < text.size()) result.append(text.data() + i, text.size() - i);
    return result;
}

// Attribute value lookup inside a single tag string, e.g. HREF="..." or src='...'.
inline std::string ExtractXmlAttribute(const std::string& tag, const std::string& attrName) {
    const std::string lower = ToLowerAsciiCopy(tag);
    const std::string lowerAttr = ToLowerAsciiCopy(attrName);
    size_t searchFrom = 0;
    while (searchFrom < lower.size()) {
        size_t attrPos = lower.find(lowerAttr, searchFrom);
        if (attrPos == std::string::npos) return "";
        // Reject matches embedded inside a longer identifier (e.g. HREFLANG).
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
        return DecodeXmlEntities( std::string_view(tag).substr(cursor + 1, end - cursor - 1));
    }
    return "";
}

// First element content lookup inside a block, e.g. <title>...</title>.
inline std::string ExtractXmlElement(std::string_view block, std::string_view lower, std::string_view tagName) {
    char openBuf[64];
    char closeBuf[64];
    std::string heapOpen, heapClose;
    std::string_view openTag, closeTag;
    if (tagName.size() <= 60) {
        openBuf[0] = '<';
        closeBuf[0] = '<';
        closeBuf[1] = '/';
        for (size_t i = 0; i < tagName.size(); ++i) {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(tagName[i])));
            openBuf[1 + i] = c;
            closeBuf[2 + i] = c;
        }
        openTag = std::string_view(openBuf, tagName.size() + 1);
        closeTag = std::string_view(closeBuf, tagName.size() + 2);
    } else {
        const std::string lowerTag = ToLowerAsciiCopy(std::string(tagName));
        heapOpen = "<" + lowerTag;
        heapClose = "</" + lowerTag;
        openTag = heapOpen;
        closeTag = heapClose;
    }
    size_t searchFrom = 0;
    while (searchFrom < lower.size()) {
        size_t open = lower.find(openTag, searchFrom);
        if (open == std::string_view::npos) return "";
        // Reject longer tag names sharing the prefix (e.g. <titler> for <title>).
        size_t after = open + openTag.size();
        if (after < lower.size() && lower[after] != '>' && lower[after] != ' ' && lower[after] != '\t' && lower[after] != '\r' && lower[after] != '\n' && lower[after] != '/') {
            searchFrom = open + 1; continue;
        }
        size_t close = lower.find('>', after);
        if (close == std::string_view::npos) return "";
        if (close > 0 && lower[close - 1] == '/') return ""; // Self-closing element has no content
        size_t contentStart = close + 1;
        while (contentStart < block.size() && (block[contentStart] == ' ' || block[contentStart] == '\t' || block[contentStart] == '\r' || block[contentStart] == '\n')) {
            ++contentStart;
        }
        if (contentStart + 9 <= block.size() && block.substr(contentStart, 9) == "<![CDATA[") {
            size_t cdataEnd = block.find("]]>", contentStart + 9);
            if (cdataEnd == std::string_view::npos) return "";
            return DecodeXmlEntities(TrimView(block.substr(contentStart + 9, cdataEnd - (contentStart + 9))));
        }
        size_t end = lower.find(closeTag, close + 1);
        if (end == std::string_view::npos) return "";
        std::string_view inner = TrimView(block.substr(close + 1, end - close - 1));
        if (inner.size() >= 12 && inner.substr(0, 9) == "<![CDATA[" && inner.substr(inner.size() - 3) == "]]>") {
            inner = TrimView(inner.substr(9, inner.size() - 12));
        }
        return DecodeXmlEntities(inner);
    }
    return "";
}

inline std::string ExtractXmlElement(const std::string& block, std::string_view tagName) {
    const std::string lower = ToLowerAsciiCopy(block);
    return ExtractXmlElement(std::string_view(block), std::string_view(lower), tagName);
}

} // namespace Aperture::XmlLite
