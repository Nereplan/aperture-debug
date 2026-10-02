#pragma once
#include "MediaLibrary.h"
#include <cmath>
#include <limits>

namespace Aperture {
namespace SmartRuleParsing {

inline bool Number(const std::string& text, long double& out) {
    if (text.empty()) return false;
    try {
        size_t consumed = 0;
        out = std::stold(text, &consumed);
        return consumed == text.size() && std::isfinite(out) && out >= 0;
    } catch (...) { return false; }
}

inline bool RemoveSuffix(std::string& text, const char* suffix) {
    const size_t length = std::char_traits<char>::length(suffix);
    if (text.size() < length || text.compare(text.size() - length, length, suffix) != 0) return false;
    text.resize(text.size() - length);
    const auto last = text.find_last_not_of(" \t\r\n");
    text.resize(last == std::string::npos ? 0 : last + 1);
    return true;
}

inline bool Parse(SmartRuleField field, std::string text, long double& out, bool& relative) {
    relative = false;
    long double multiplier = 1;
    switch (field) {
    case SmartRuleField::Duration: {
        if (text.find(':') != std::string::npos) {
            long double total = 0;
            size_t start = 0, components = 0;
            do {
                const size_t end = text.find(':', start);
                long double part = 0;
                if (++components > 3 || !Number(text.substr(start, end == std::string::npos ? end : end - start), part)) return false;
                total = total * 60 + part;
                if (end == std::string::npos) break;
                start = end + 1;
            } while (true);
            out = total;
            return std::isfinite(out) && out <= static_cast<long double>((std::numeric_limits<int64_t>::max)());
        }
        if (RemoveSuffix(text, "hr") || RemoveSuffix(text, "h")) multiplier = 3600;
        else if (RemoveSuffix(text, "min") || RemoveSuffix(text, "m")) multiplier = 60;
        else RemoveSuffix(text, "s");
        break;
    }
    case SmartRuleField::Progress:
        if (RemoveSuffix(text, "%")) multiplier = 0.01L;
        else {
            if (!Number(text, out)) return false;
            if (out > 1) out /= 100;
            return out <= 1;
        }
        break;
    case SmartRuleField::FileSize:
        if (RemoveSuffix(text, "gb") || RemoveSuffix(text, "g")) multiplier = 1024.L * 1024 * 1024;
        else if (RemoveSuffix(text, "mb") || RemoveSuffix(text, "m")) multiplier = 1024.L * 1024;
        else if (RemoveSuffix(text, "kb") || RemoveSuffix(text, "k")) multiplier = 1024;
        break;
    case SmartRuleField::DateAdded:
    case SmartRuleField::LastWatched:
        if (RemoveSuffix(text, "days") || RemoveSuffix(text, "day") || RemoveSuffix(text, "d")) {
            relative = true; multiplier = 86400;
        }
        break;
    case SmartRuleField::VideoHeight:
        if (text == "4k") text = "2160";
        else if (text == "2k") text = "1440";
        else if (text == "fhd") text = "1080";
        else if (text == "hd") text = "720";
        else if (text == "sd") text = "480";
        else RemoveSuffix(text, "p");
        break;
    default: return false;
    }
    if (!Number(text, out)) return false;
    out *= multiplier;
    if (!std::isfinite(out)) return false;
    if (field == SmartRuleField::Progress) return out <= 1;
    if (field == SmartRuleField::VideoHeight)
        return out <= (std::numeric_limits<uint32_t>::max)() && std::floor(out) == out;
    if (field == SmartRuleField::FileSize)
        return out < std::ldexp(1.0L, 64); // MSVC long double is double; never cast 2^64.
    return out < std::ldexp(1.0L, 63);
}

} // namespace SmartRuleParsing
} // namespace Aperture
