#include "Json.h"
#include "Common.h"
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <limits>

namespace Aperture {

static const JsonValue s_nullValue;

JsonValue::JsonValue(const std::wstring& ws) : value(Utf16ToUtf8(ws)) {}

std::wstring JsonValue::as_wstring(const std::wstring& def) const { return is_string() ? Utf8ToUtf16(as_string_ref()) : def; }

const std::string& JsonValue::as_string_ref() const {
    static const std::string empty;
    return is_string() ? std::get<std::string>(value) : empty;
}

const JsonValue::ArrayType& JsonValue::as_array() const {
    static const ArrayType empty;
    return is_array() ? std::get<ArrayType>(value) : empty;
}

JsonValue::ArrayType& JsonValue::as_array() {
    if (!is_array()) value.emplace<ArrayType>();
    return std::get<ArrayType>(value);
}

const JsonValue::ObjectType& JsonValue::as_object() const {
    static const ObjectType empty;
    return is_object() ? std::get<ObjectType>(value) : empty;
}

JsonValue::ObjectType& JsonValue::as_object() {
    if (!is_object()) value.emplace<ObjectType>();
    return std::get<ObjectType>(value);
}

const JsonValue& JsonValue::get(std::string_view key) const {
    if (!is_object()) return s_nullValue;
    const auto& objectVal = as_object();
    auto i = objectVal.find(key);
    return i == objectVal.end() ? s_nullValue : i->second;
}

JsonValue& JsonValue::operator[](const std::string& key) {
    return as_object()[key];
}

JsonValue& JsonValue::operator[](size_t i) {
    auto& arrayVal = as_array();
    if (i >= arrayVal.size()) arrayVal.resize(i + 1);
    return arrayVal[i];
}

const JsonValue& JsonValue::operator[](size_t i) const { return is_array() && i < as_array().size() ? as_array()[i] : s_nullValue; }

static void AppendSpaces(std::string& out, size_t count) {
    out.append(count, ' ');
}

static void EscapeJsonString(const std::string& in, std::string& out) {
    static constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    size_t runStart = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        const char* escape = nullptr;
        switch (c) {
            case '"': escape = "\\\""; break;
            case '\\': escape = "\\\\"; break;
            case '\b': escape = "\\b"; break;
            case '\f': escape = "\\f"; break;
            case '\n': escape = "\\n"; break;
            case '\r': escape = "\\r"; break;
            case '\t': escape = "\\t"; break;
            default:
                if (c < 0x20) {
                    if (i > runStart) out.append(in, runStart, i - runStart);
                    out.append("\\u00");
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 0x0f]);
                    runStart = i + 1;
                }
                break;
        }
        if (escape) {
            if (i > runStart) out.append(in, runStart, i - runStart);
            out.append(escape);
            runStart = i + 1;
        }
    }
    if (runStart < in.size()) out.append(in, runStart, in.size() - runStart);
    out.push_back('"');
}

std::string JsonValue::stringify(bool pretty, int indent) const {
    std::string out;
    append_to(out, pretty, indent);
    return out;
}

void JsonValue::append_to(std::string& out, bool pretty, int indent) const {
    WriteJson(out, pretty, indent);
}

void JsonValue::WriteJson(std::string& o, bool pretty, int indent) const {
    const size_t pad = pretty ? static_cast<size_t>(indent * 2) : 0;
    const size_t child = pretty ? static_cast<size_t>((indent + 1) * 2) : 0;
    switch (GetType()) {
        case JsonType::Null: o.append("null"); break;
        case JsonType::Boolean: o.append(as_bool() ? "true" : "false"); break;
        case JsonType::Number: {
            const double numberVal = as_number();
            if (!std::isfinite(numberVal)) {
                o.append("null");
            } else if (std::floor(numberVal) == numberVal && numberVal >= (double)std::numeric_limits<int64_t>::min() &&
                       numberVal < std::ldexp(1.0, 63)) {
                char buffer[32];
                const auto result = std::to_chars(buffer, buffer + sizeof(buffer), (int64_t)numberVal);
                o.append(buffer, result.ptr);
            } else {
                // Preserve the store's six significant digits without allocating
                // a stream (and without inheriting a non-JSON decimal separator).
                char buffer[32];
                const auto result = std::to_chars(buffer, buffer + sizeof(buffer), numberVal,
                    std::chars_format::general, 6);
                o.append(buffer, result.ptr);
            }
            break;
        }
        case JsonType::String:
            EscapeJsonString(as_string_ref(), o);
            break;
        case JsonType::Array: {
            const auto& arrayVal = as_array();
            if (arrayVal.empty()) { o.append("[]"); break; }
            o.append(pretty ? "[\n" : "[");
            for (size_t i = 0; i < arrayVal.size(); ++i) {
                if (pretty) AppendSpaces(o, child);
                arrayVal[i].WriteJson(o, pretty, indent + 1);
                if (i + 1 < arrayVal.size()) o.push_back(',');
                if (pretty) o.push_back('\n');
            }
            if (pretty) AppendSpaces(o, pad);
            o.push_back(']');
            break;
        }
        case JsonType::Object: {
            const auto& objectVal = as_object();
            if (objectVal.empty()) {
                o.append("{}");
                break;
            }
            o.append(pretty ? "{\n" : "{");
            size_t count = 0;
            for (auto it = objectVal.begin(); it != objectVal.end(); ++it, ++count) {
                if (pretty) AppendSpaces(o, child);
                EscapeJsonString(it->first, o);
                o.append(pretty ? ": " : ":");
                it->second.WriteJson(o, pretty, indent + 1);
                if (count + 1 < objectVal.size()) o.push_back(',');
                if (pretty) o.push_back('\n');
            }
            if (pretty) AppendSpaces(o, pad);
            o.push_back('}');
            break;
        }
    }
}

class JsonParser {
    const std::string& s;
    size_t p = 0, depth = 0;
    bool bad = false;
    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) { ++p; }
    }
    bool take(char c) {
        ws();
        if (p >= s.size() || s[p] != c) { bad = true; return false; }
        ++p;
        return true;
    }
    bool hex(unsigned& x) {
        x = 0;
        for (int i = 0; i < 4; ++i) {
            if (p >= s.size()) return false;
            unsigned char c = (unsigned char)s[p++];
            if (c >= '0' && c <= '9') x = x * 16 + c - '0';
            else if (c >= 'a' && c <= 'f') x = x * 16 + c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') x = x * 16 + c - 'A' + 10;
            else return false;
        }
        return true;
    }
    static void utf8(std::string& o, uint32_t c) {
        if (c <= 0x7f) { o.push_back((char)c); } else if (c <= 0x7ff) {
            o.push_back((char)(0xc0 | (c >> 6)));
            o.push_back((char)(0x80 | (c & 63)));
        } else if (c <= 0xffff) {
            o.push_back((char)(0xe0 | (c >> 12)));
            o.push_back((char)(0x80 | ((c >> 6) & 63)));
            o.push_back((char)(0x80 | (c & 63)));
        } else if (c <= 0x10ffff) {
            o.push_back((char)(0xf0 | (c >> 18)));
            o.push_back((char)(0x80 | ((c >> 12) & 63)));
            o.push_back((char)(0x80 | ((c >> 6) & 63)));
            o.push_back((char)(0x80 | (c & 63)));
        }
    }
    std::string str() {
        std::string o;
        if (!take('"')) return o;
        while (p < s.size()) {
            // Copy ordinary UTF-8 in one span. Most keys/titles need one
            // allocation, instead of growing a string one byte at a time.
            const size_t start = p;
            while (p < s.size() && static_cast<unsigned char>(s[p]) >= 0x20 && s[p] != '"' && s[p] != '\\') ++p;
            const size_t length = p - start;
            if (length > JsonValue::kMaxJsonStringBytes - o.size()) { bad = true; return {}; }
            o.append(s, start, length);
            if (p == s.size()) break;
            const unsigned char c = static_cast<unsigned char>(s[p++]);
            if (c == '"') return o;
            if (c < 0x20 || o.size() == JsonValue::kMaxJsonStringBytes) { bad = true; return {}; }
            if (p >= s.size()) {
                bad = true;
                return {};
            }
            char e = s[p++];
            switch (e) {
                case '"': o.push_back('"'); break;
                case '\\': o.push_back('\\'); break;
                case '/': o.push_back('/'); break;
                case 'b': o.push_back('\b'); break;
                case 'f': o.push_back('\f'); break;
                case 'n': o.push_back('\n'); break;
                case 'r': o.push_back('\r'); break;
                case 't': o.push_back('\t'); break;
                case 'u': {
                    unsigned h = 0;
                    if (!hex(h)) {
                        bad = true;
                        return {};
                    }
                    uint32_t codePoint = h;
                    if (h >= 0xd800 && h <= 0xdbff) {
                        if (p + 1 >= s.size() || s[p] != '\\' || s[p + 1] != 'u') {
                            bad = true;
                            return {};
                        }
                        p += 2;
                        unsigned l = 0;
                        if (!hex(l) || l < 0xdc00 || l > 0xdfff) {
                            bad = true;
                            return {};
                        }
                        codePoint = 0x10000 + ((h - 0xd800) << 10) + (l - 0xdc00);
                    } else if (h >= 0xdc00 && h <= 0xdfff) {
                        bad = true;
                        return {};
                    }
                    utf8(o, codePoint);
                    if (o.size() > JsonValue::kMaxJsonStringBytes) { bad = true; return {}; }
                    break;
                }
                default: bad = true; return {};
            }
        }
        bad = true;
        return {};
    }
    JsonValue val() {
        ws();
        if (p >= s.size()) {
            bad = true;
            return {};
        }
        char c = s[p];
        if (c == '"') { return JsonValue(str()); }
        if (c == '{') {
            if (depth >= JsonValue::kMaxJsonDepth) {
                bad = true;
                return {};
            }
            ++depth;
            ++p;
            JsonValue o = JsonValue::Object();
            ws();
            if (p < s.size() && s[p] == '}') { ++p; --depth; return o; }
            while (!bad) {
                ws();
                if (p >= s.size() || s[p] != '"') { bad = true; break; }
                std::string k = str();
                if (bad || !take(':')) break;
                JsonValue v = val();
                if (bad) break;
                if (o.as_object().size() >= JsonValue::kMaxJsonContainerElements) { bad = true; break; }
                // Aperture stores keys in sorted order. An end hint makes
                // that common insertion linear, while arbitrary input and
                // duplicate keys retain normal last-value-wins semantics.
                auto& fields = o.as_object();
                fields.insert_or_assign(fields.end(), std::move(k), std::move(v));
                ws();
                if (p < s.size() && s[p] == '}') { ++p; --depth; return o; }
                if (!take(',')) break;
            }
            --depth;
            return {};
        }
        if (c == '[') {
            if (depth >= JsonValue::kMaxJsonDepth) {
                bad = true;
                return {};
            }
            ++depth;
            ++p;
            JsonValue a = JsonValue::Array();
            ws();
            if (p < s.size() && s[p] == ']') { ++p; --depth; return a; }
            while (!bad) {
                if (a.as_array().size() >= JsonValue::kMaxJsonContainerElements) { bad = true; break; }
                JsonValue item = val();
                if (bad) break;
                auto& elements = a.as_array();
                if (elements.size() == elements.capacity()) {
                    // MSVC's map move can allocate, so vector growth otherwise
                    // deep-copies every existing JSON subtree. This unpublished
                    // parser array can transfer ownership explicitly: if a move
                    // throws, the entire parse is abandoned anyway. Keep the
                    // usual 1.5x growth without exceeding the element limit.
                    const size_t capacity = elements.capacity();
                    const size_t next = (std::min)(JsonValue::kMaxJsonContainerElements,
                        (std::max)(capacity + capacity / 2, elements.size() + 1));
                    JsonValue::ArrayType grown;
                    grown.reserve(next);
                    for (auto& element : elements) grown.push_back(std::move(element));
                    elements.swap(grown);
                }
                elements.push_back(std::move(item));
                ws();
                if (p < s.size() && s[p] == ']') { ++p; --depth; return a; }
                if (!take(',')) break;
            }
            --depth;
            return {};
        }
        if (s.compare(p, 4, "true") == 0) { p += 4; return JsonValue(true); }
        if (s.compare(p, 5, "false") == 0) { p += 5; return JsonValue(false); }
        if (s.compare(p, 4, "null") == 0) {
            p += 4;
            return {};
        }
        // Numbers
        size_t st = p;
        if (s[p] == '-') ++p;
        if (p >= s.size()) {
            bad = true;
            return {};
        }
        if (s[p] == '0') { ++p; } else if (s[p] >= '1' && s[p] <= '9') {
            while (p < s.size() && std::isdigit((unsigned char)s[p])) ++p;
        } else {
            bad = true;
            return {};
        }
        if (p < s.size() && s[p] == '.') {
            ++p;
            size_t q = p;
            while (p < s.size() && std::isdigit((unsigned char)s[p])) ++p;
            if (p == q) {
                bad = true;
                return {};
            }
        }
        if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
            ++p;
            if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
            size_t q = p;
            while (p < s.size() && std::isdigit((unsigned char)s[p])) ++p;
            if (p == q) {
                bad = true;
                return {};
            }
        }
        // Parse directly from the bounded input; no temporary substring or
        // locale machinery on the ordinary path. Preserve the legacy CRT's
        // range handling for rare subnormal/underflow values.
        double n = 0.0;
        const auto parsed = std::from_chars(s.data() + st, s.data() + p, n, std::chars_format::general);
        if (parsed.ec == std::errc::result_out_of_range ||
            (parsed.ec == std::errc{} && n != 0.0 && std::abs(n) < (std::numeric_limits<double>::min)())) {
            errno = 0;
            char* end = nullptr;
            const std::string number = s.substr(st, p - st);
            n = std::strtod(number.c_str(), &end);
            if (errno == ERANGE || end != number.c_str() + number.size() || !std::isfinite(n)) { bad = true; return {}; }
        } else if (parsed.ec != std::errc{} || parsed.ptr != s.data() + p || !std::isfinite(n)) {
            bad = true;
            return {};
        }
        return JsonValue(n);
    }

public:
    explicit JsonParser(const std::string& x) : s(x) {}
    bool consumeArray(const std::function<void(JsonValue&)>& consume) {
        if (s.empty() || s.size() > JsonValue::kMaxJsonInputBytes || !consume || !take('[')) return false;
        depth = 1; // The streamed outer array counts toward the nesting limit.
        ws();
        if (p < s.size() && s[p] == ']') { ++p; ws(); return p == s.size(); }
        size_t count = 0;
        while (!bad) {
            if (count == JsonValue::kMaxJsonContainerElements) return false;
            JsonValue item = val();
            if (bad) return false;
            consume(item);
            ++count;
            ws();
            if (p < s.size() && s[p] == ']') { ++p; ws(); return p == s.size(); }
            if (!take(',')) return false;
        }
        return false;
    }
    bool run(JsonValue& out) {
        if (s.size() > JsonValue::kMaxJsonInputBytes) { return false; }
        out = val();
        ws();
        return !bad && p == s.size();
    }
};

bool JsonValue::consume_array(const std::string& text, const std::function<void(JsonValue&)>& consume) {
    return JsonParser(text).consumeArray(consume);
}

bool JsonValue::parse(const std::string& text, JsonValue& out) {
    if (text.empty() || text.size() > kMaxJsonInputBytes) { out = JsonValue(); return false; }
    JsonParser p(text);
    if (!p.run(out)) { out = JsonValue(); return false; }
    return true;
}

JsonValue JsonValue::parse(const std::string& text) { JsonValue out; parse(text, out); return out; }
} // namespace Aperture
