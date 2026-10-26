#pragma once

// Shared minimal XML text helpers: entity decoding and case-insensitive
// element/attribute extraction for the lightweight XML dialects the player
// reads (XSPF/ASX/WPL playlists, RSS podcast feeds). Header-only so any
// translation unit can parse XML without a dedicated XML dependency.

#include <cctype>
#include <string>

namespace Aperture::XmlLite {

// Helper: Trim string
inline std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

inline std::string ToLowerAsciiCopy(std::string str) {
    for (auto& c : str) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return str;
}

inline std::string DecodeXmlEntities(const std::string& text) {
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
        return DecodeXmlEntities(tag.substr(cursor + 1, end - cursor - 1));
    }
    return "";
}

// First element content lookup inside a block, e.g. <title>...</title>.
inline std::string ExtractXmlElement(const std::string& block, const std::string& tagName) {
    const std::string lower = ToLowerAsciiCopy(block);
    const std::string lowerTag = ToLowerAsciiCopy(tagName);
    size_t searchFrom = 0;
    while (searchFrom < lower.size()) {
        size_t open = lower.find("<" + lowerTag, searchFrom);
        if (open == std::string::npos) return "";
        // Reject longer tag names sharing the prefix (e.g. <titler> for <title>).
        size_t after = open + 1 + lowerTag.size();
        if (after < lower.size() && lower[after] != '>' && lower[after] != ' ' && lower[after] != '\t' && lower[after] != '\r' && lower[after] != '\n' && lower[after] != '/') {
            searchFrom = open + 1; continue;
        }
        size_t close = lower.find('>', after);
        if (close == std::string::npos) return "";
        if (close > 0 && lower[close - 1] == '/') return ""; // Self-closing element has no content
        size_t contentStart = close + 1;
        while (contentStart < block.size() && (block[contentStart] == ' ' || block[contentStart] == '\t' || block[contentStart] == '\r' || block[contentStart] == '\n')) {
            ++contentStart;
        }
        if (contentStart + 9 <= block.size() && block.compare(contentStart, 9, "<![CDATA[") == 0) {
            size_t cdataEnd = block.find("]]>", contentStart + 9);
            if (cdataEnd == std::string::npos) return "";
            return DecodeXmlEntities(Trim(block.substr(contentStart + 9, cdataEnd - (contentStart + 9))));
        }
        size_t end = lower.find("</" + lowerTag, close + 1);
        if (end == std::string::npos) return "";
        std::string inner = Trim(block.substr(close + 1, end - close - 1));
        if (inner.size() >= 12 && inner.rfind("<![CDATA[", 0) == 0 && inner.compare(inner.size() - 3, 3, "]]>") == 0) {
            inner = Trim(inner.substr(9, inner.size() - 12));
        }
        return DecodeXmlEntities(inner);
    }
    return "";
}

} // namespace Aperture::XmlLite
