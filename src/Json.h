#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <sstream>
#include <cstdint>
#include <cmath>
#include <limits>
#include <utility>

namespace Aperture {

enum class JsonType { Null, Boolean, Number, String, Array, Object };

class JsonValue {
public:
    static constexpr size_t kMaxJsonInputBytes = 64 * 1024 * 1024; // 64 MB max input
    static constexpr size_t kMaxJsonDepth = 128;                   // Max 128 nesting levels
    static constexpr size_t kMaxJsonContainerElements = 1000000;   // Max 1M elements
    static constexpr size_t kMaxJsonStringBytes = 16 * 1024 * 1024;// Max 16MB per string
    JsonType type = JsonType::Null;
    bool boolVal = false;
    double numberVal = 0.0;
    std::string stringVal;
    std::vector<JsonValue> arrayVal;
    std::map<std::string, JsonValue> objectVal;
    JsonValue() : type(JsonType::Null) {}
    JsonValue(bool b) : type(JsonType::Boolean), boolVal(b) {}
    JsonValue(int i) : type(JsonType::Number), numberVal(i) {}
    JsonValue(int64_t i) : type(JsonType::Number), numberVal((double)i) {}
    JsonValue(uint64_t u) : type(JsonType::Number), numberVal((double)u) {}
    JsonValue(double d) : type(JsonType::Number), numberVal(d) {}
    JsonValue(const char* s) : type(JsonType::String), stringVal(s ? s : "") {}
    JsonValue(const std::string& s) : type(JsonType::String), stringVal(s) {}
    JsonValue(std::string&& s) : type(JsonType::String), stringVal(std::move(s)) {}
    JsonValue(const std::wstring& ws);
    static JsonValue Array() { JsonValue v; v.type = JsonType::Array; return v; }
    static JsonValue Object() { JsonValue v; v.type = JsonType::Object; return v; }
    bool is_null() const { return type == JsonType::Null; }
    bool is_bool() const { return type == JsonType::Boolean; }
    bool is_number() const { return type == JsonType::Number; }
    bool is_string() const { return type == JsonType::String; }
    bool is_array() const { return type == JsonType::Array; }
    bool is_object() const { return type == JsonType::Object; }
    bool as_bool(bool def = false) const { return is_bool() ? boolVal : def; }
    double as_number(double def = 0.0) const { return is_number() ? numberVal : def; }
    int as_int(int def = 0) const { return is_number() && std::isfinite(numberVal) && numberVal >= (double)std::numeric_limits<int>::min() && numberVal <= (double)std::numeric_limits<int>::max() ? (int)numberVal : def; }
    int64_t as_int64(int64_t def = 0) const { return is_number() && std::isfinite(numberVal) && numberVal >= (double)std::numeric_limits<int64_t>::min() && numberVal <= (double)std::numeric_limits<int64_t>::max() ? (int64_t)numberVal : def; }
    uint64_t as_uint64(uint64_t def = 0) const { return is_number() && std::isfinite(numberVal) && numberVal >= 0 && numberVal <= (double)std::numeric_limits<uint64_t>::max() ? (uint64_t)numberVal : def; }
    float as_float(float def = 0.0f) const { return is_number() ? (float)numberVal : def; }
    std::string as_string(const std::string& def = "") const { return is_string() ? stringVal : def; }
    std::wstring as_wstring(const std::wstring& def = L"") const;
    const std::vector<JsonValue>& as_array() const { return arrayVal; }
    std::vector<JsonValue>& as_array() { return arrayVal; }
    const std::map<std::string, JsonValue>& as_object() const { return objectVal; }
    std::map<std::string, JsonValue>& as_object() { return objectVal; }
    bool has_key(const std::string& key) const { if (!is_object()) return false; return objectVal.find(key) != objectVal.end(); }
    const JsonValue& get(const std::string& key) const;
    JsonValue& operator[](const std::string& key);
    JsonValue& operator[](size_t index);
    const JsonValue& operator[](size_t index) const;
    void push_back(const JsonValue& item) { if (type != JsonType::Array) type = JsonType::Array; arrayVal.push_back(item); }
    void push_back(JsonValue&& item) { if (type != JsonType::Array) type = JsonType::Array; arrayVal.push_back(std::move(item)); }
    std::string stringify(bool pretty = false, int indent = 0) const;
    static JsonValue parse(const std::string& jsonText);
    // Strict variant: returns false for malformed/truncated JSON. The legacy
    // parse() entry point remains source-compatible and returns null on error.
    static bool parse(const std::string& jsonText, JsonValue& out);

private:
    void WriteJson(std::ostringstream& out, bool pretty, int indent) const;
};

} // namespace Aperture
