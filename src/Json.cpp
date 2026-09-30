#include "Json.h"
#include "Common.h"
#include <cctype>
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

static void EscapeJsonString(const std::string& in, std::ostringstream& o) {
    o << '"';
    for (unsigned char c : in) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\b': o << "\\b"; break;
            case '\f': o << "\\f"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[7];
                    sprintf_s(b, "\\u%04x", (unsigned)c);
                    o << b;
                } else { o << (char)c; }
                break;
        }
    }
    o << '"';
}

std::string JsonValue::stringify(bool pretty, int indent) const { std::ostringstream o; WriteJson(o, pretty, indent); return o.str(); }

void JsonValue::WriteJson(std::ostringstream& o, bool pretty, int indent) const {
    std::string pad = pretty ? std::string(indent * 2, ' ') : std::string();
    std::string child = pretty ? std::string((indent + 1) * 2, ' ') : std::string();
    switch (GetType()) {
        case JsonType::Null: o << "null"; break;
        case JsonType::Boolean: o << (as_bool() ? "true" : "false"); break;
        case JsonType::Number: {
            const double numberVal = as_number();
            if (!std::isfinite(numberVal)) {
                o << "null";
            } else if (std::floor(numberVal) == numberVal && numberVal >= (double)std::numeric_limits<int64_t>::min() &&
                       numberVal <= (double)std::numeric_limits<int64_t>::max()) {
                o << (int64_t)numberVal;
            } else { o << numberVal; }
            break;
        }
        case JsonType::String:
            EscapeJsonString(as_string_ref(), o);
            break;
        case JsonType::Array: {
            const auto& arrayVal = as_array();
            if (arrayVal.empty()) { o << "[]"; break; }
            o << (pretty ? "[\n" : "[");
            for (size_t i = 0; i < arrayVal.size(); ++i) {
                if (pretty) o << child;
                arrayVal[i].WriteJson(o, pretty, indent + 1);
                if (i + 1 < arrayVal.size()) o << ',';
                if (pretty) o << '\n';
            }
            if (pretty) o << pad;
            o << ']';
            break;
        }
        case JsonType::Object: {
            const auto& objectVal = as_object();
            if (objectVal.empty()) {
                o << "{}";
                break;
            }
            o << (pretty ? "{\n" : "{");
            size_t count = 0;
            for (auto it = objectVal.begin(); it != objectVal.end(); ++it, ++count) {
                if (pretty) o << child;
                EscapeJsonString(it->first, o);
                o << (pretty ? ": " : ":");
                it->second.WriteJson(o, pretty, indent + 1);
                if (count + 1 < objectVal.size()) o << ',';
                if (pretty) o << '\n';
            }
            if (pretty) o << pad;
            o << '}';
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
            unsigned char c = (unsigned char)s[p++];
            if (c == '"') return o;
            if (c < 0x20) {
                bad = true;
                return {};
            }
            if (o.size() >= JsonValue::kMaxJsonStringBytes) {
                bad = true;
                return {};
            }
            if (c != '\\') { o.push_back((char)c); continue; }
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
                o.as_object()[std::move(k)] = std::move(v);
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
                a.push_back(val());
                if (bad) break;
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
        errno = 0;
        char* end = nullptr;
        std::string numStr = s.substr(st, p - st);
        double n = std::strtod(numStr.c_str(), &end);
        if (errno == ERANGE || !std::isfinite(n)) {
            bad = true;
            return {};
        }
        return JsonValue(n);
    }

public:
    explicit JsonParser(const std::string& x) : s(x) {}
    bool run(JsonValue& out) {
        if (s.size() > JsonValue::kMaxJsonInputBytes) { return false; }
        out = val();
        ws();
        return !bad && p == s.size();
    }
};

bool JsonValue::parse(const std::string& text, JsonValue& out) {
    if (text.empty() || text.size() > kMaxJsonInputBytes) { out = JsonValue(); return false; }
    JsonParser p(text);
    if (!p.run(out)) { out = JsonValue(); return false; }
    return true;
}

JsonValue JsonValue::parse(const std::string& text) { JsonValue out; parse(text, out); return out; }
} // namespace Aperture
