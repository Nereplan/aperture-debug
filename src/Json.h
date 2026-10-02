#pragma once
#include "StorageLimits.h"

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <sstream>
#include <cstdint>
#include <cmath>
#include <limits>
#include <utility>
#include <variant>
#include <string_view>

namespace Aperture {

enum class JsonType { Null, Boolean, Number, String, Array, Object };

class JsonValue {
public:
    static constexpr size_t kMaxJsonInputBytes = kMaxPortableStoreBytes;
    static constexpr size_t kMaxJsonDepth = 128;                   // Max 128 nesting levels
    static constexpr size_t kMaxJsonContainerElements = 1000000;   // Max 1M elements
    static constexpr size_t kMaxJsonStringBytes = 16 * 1024 * 1024;// Max 16MB per string
    using ArrayType = std::vector<JsonValue>;
    using ObjectType = std::map<std::string, JsonValue, std::less<>>;
    JsonValue() = default;
    JsonValue(bool b) : value(b) {}
    JsonValue(int i) : value((double)i) {}
    JsonValue(int64_t i) : value((double)i) {}
    JsonValue(uint64_t u) : value((double)u) {}
    JsonValue(double d) : value(d) {}
    JsonValue(const char* s) : value(std::string(s ? s : "")) {}
    JsonValue(const std::string& s) : value(s) {}
    JsonValue(std::string&& s) : value(std::move(s)) {}
    JsonValue(const std::wstring& ws);
    static JsonValue Array() { JsonValue v; v.value.emplace<ArrayType>(); return v; }
    static JsonValue Object() { JsonValue v; v.value.emplace<ObjectType>(); return v; }
    JsonType GetType() const { return static_cast<JsonType>(value.index()); }
    bool is_null() const { return GetType() == JsonType::Null; }
    bool is_bool() const { return GetType() == JsonType::Boolean; }
    bool is_number() const { return GetType() == JsonType::Number; }
    bool is_string() const { return GetType() == JsonType::String; }
    bool is_array() const { return GetType() == JsonType::Array; }
    bool is_object() const { return GetType() == JsonType::Object; }
    bool as_bool(bool def = false) const { return is_bool() ? std::get<bool>(value) : def; }
    double as_number(double def = 0.0) const { return is_number() ? std::get<double>(value) : def; }
    int as_int(int def = 0) const { const double n = as_number(); return is_number() && std::isfinite(n) && n >= (double)std::numeric_limits<int>::min() && n <= (double)std::numeric_limits<int>::max() ? (int)n : def; }
    int64_t as_int64(int64_t def = 0) const { const double n = as_number(); return is_number() && std::isfinite(n) && n >= -std::ldexp(1.0, 63) && n < std::ldexp(1.0, 63) ? (int64_t)n : def; }
    uint64_t as_uint64(uint64_t def = 0) const { const double n = as_number(); return is_number() && std::isfinite(n) && n >= 0 && n < std::ldexp(1.0, 64) ? (uint64_t)n : def; }
    float as_float(float def = 0.0f) const { return is_number() ? (float)as_number() : def; }
    std::string as_string(const std::string& def = "") const { return is_string() ? std::get<std::string>(value) : def; }
    const std::string& as_string_ref() const;
    std::wstring as_wstring(const std::wstring& def = L"") const;
    const ArrayType& as_array() const;
    ArrayType& as_array();
    const ObjectType& as_object() const;
    ObjectType& as_object();
    bool has_key(std::string_view key) const { return is_object() && as_object().find(key) != as_object().end(); }
    const JsonValue& get(std::string_view key) const;
    JsonValue& operator[](const std::string& key);
    JsonValue& operator[](size_t index);
    const JsonValue& operator[](size_t index) const;
    void push_back(const JsonValue& item) { as_array().push_back(item); }
    void push_back(JsonValue&& item) { as_array().push_back(std::move(item)); }
    void append_to(std::string& out, bool pretty = false, int indent = 0) const;
    std::string stringify(bool pretty = false, int indent = 0) const;
    static JsonValue parse(const std::string& jsonText);
    // Strict variant: returns false for malformed/truncated JSON. The legacy
    // parse() entry point remains source-compatible and returns null on error.
    static bool parse(const std::string& jsonText, JsonValue& out);

private:
    // Only the active payload exists. Scalars no longer construct empty maps
    // and vectors; copies/moves retain ordinary owning value semantics.
    std::variant<std::monostate, bool, double, std::string, ArrayType, ObjectType> value;
    void WriteJson(std::string& out, bool pretty, int indent) const;
};

} // namespace Aperture
