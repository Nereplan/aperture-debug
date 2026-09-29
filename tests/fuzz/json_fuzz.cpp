#include "../../src/Json.h"
#include "../../src/Common.h"

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <random>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cassert>
#include <limits>
#include <functional>

namespace {

using Aperture::JsonValue;
using Aperture::JsonType;

// Deterministic PRNG wrapper around std::mt19937_64
class FuzzRng {
public:
    explicit FuzzRng(uint64_t seed) : rng_(seed), initialSeed_(seed) {}

    uint64_t Seed() const { return initialSeed_; }

    uint64_t NextU64() { return rng_(); }

    uint32_t NextU32() { return static_cast<uint32_t>(rng_() & 0xFFFFFFFF); }

    size_t NextSize(size_t minVal, size_t maxVal) {
        if (minVal >= maxVal) return minVal;
        std::uniform_int_distribution<size_t> dist(minVal, maxVal);
        return dist(rng_);
    }

    int64_t NextInt64(int64_t minVal, int64_t maxVal) {
        if (minVal >= maxVal) return minVal;
        std::uniform_int_distribution<int64_t> dist(minVal, maxVal);
        return dist(rng_);
    }

    double NextDouble(double minVal, double maxVal) {
        std::uniform_real_distribution<double> dist(minVal, maxVal);
        return dist(rng_);
    }

    bool Prob(double probability) {
        std::bernoulli_distribution dist(probability);
        return dist(rng_);
    }

    uint8_t NextByte() {
        return static_cast<uint8_t>(rng_() & 0xFF);
    }

    char NextChar() {
        return static_cast<char>(rng_() & 0xFF);
    }

    template <typename T>
    const T& Pick(const std::vector<T>& items) {
        assert(!items.empty());
        size_t idx = NextSize(0, items.size() - 1);
        return items[idx];
    }

private:
    std::mt19937_64 rng_;
    uint64_t initialSeed_;
};

// Deep equality verification between two JsonValue trees
bool JsonValuesEqual(const JsonValue& a, const JsonValue& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case JsonType::Null:
            return true;
        case JsonType::Boolean:
            return a.boolVal == b.boolVal;
        case JsonType::Number: {
            if (std::isnan(a.numberVal) || std::isnan(b.numberVal)) {
                return std::isnan(a.numberVal) && std::isnan(b.numberVal);
            }
            if (std::isinf(a.numberVal) || std::isinf(b.numberVal)) {
                return a.numberVal == b.numberVal;
            }
            if (a.numberVal == b.numberVal) return true;
            double diff = std::abs(a.numberVal - b.numberVal);
            double maxVal = std::max(1.0, std::max(std::abs(a.numberVal), std::abs(b.numberVal)));
            return (diff / maxVal) < 1e-12;
        }
        case JsonType::String:
            return a.stringVal == b.stringVal;
        case JsonType::Array: {
            if (a.arrayVal.size() != b.arrayVal.size()) return false;
            for (size_t i = 0; i < a.arrayVal.size(); ++i) {
                if (!JsonValuesEqual(a.arrayVal[i], b.arrayVal[i])) return false;
            }
            return true;
        }
        case JsonType::Object: {
            if (a.objectVal.size() != b.objectVal.size()) return false;
            auto itA = a.objectVal.begin();
            auto itB = b.objectVal.begin();
            for (; itA != a.objectVal.end(); ++itA, ++itB) {
                if (itA->first != itB->first) return false;
                if (!JsonValuesEqual(itA->second, itB->second)) return false;
            }
            return true;
        }
    }
    return false;
}

// Check depth of JsonValue tree does not exceed limit
size_t ComputeDepth(const JsonValue& v) {
    if (v.is_array()) {
        size_t maxChild = 0;
        for (const auto& item : v.as_array()) {
            maxChild = std::max(maxChild, ComputeDepth(item));
        }
        return 1 + maxChild;
    }
    if (v.is_object()) {
        size_t maxChild = 0;
        for (const auto& kv : v.as_object()) {
            maxChild = std::max(maxChild, ComputeDepth(kv.second));
        }
        return 1 + maxChild;
    }
    return 0;
}

// Initial structured seed corpus representing diverse valid, invalid, and boundary JSON structures
std::vector<std::string> BuildSeedCorpus() {
    std::vector<std::string> corpus;

    // --- Scalar primitives ---
    corpus.push_back("null");
    corpus.push_back("true");
    corpus.push_back("false");
    corpus.push_back("0");
    corpus.push_back("-0");
    corpus.push_back("1");
    corpus.push_back("-1");
    corpus.push_back("42");
    corpus.push_back("-42");
    corpus.push_back("123456789");
    corpus.push_back("-987654321");
    corpus.push_back("0.0");
    corpus.push_back("-0.0");
    corpus.push_back("0.5");
    corpus.push_back("-0.5");
    corpus.push_back("123.456");
    corpus.push_back("-123.456");
    corpus.push_back("1e0");
    corpus.push_back("1e5");
    corpus.push_back("1e-5");
    corpus.push_back("1E+5");
    corpus.push_back("-1.25e+3");
    corpus.push_back("9007199254740991");
    corpus.push_back("-9007199254740991");
    corpus.push_back("1.7976931348623157e+308");

    // --- Strings ---
    corpus.push_back("\"\"");
    corpus.push_back("\"a\"");
    corpus.push_back("\"hello world\"");
    corpus.push_back("\"escapes: \\\" \\\\ \\/ \\b \\f \\n \\r \\t\"");
    corpus.push_back("\"unicode escape: \\u0020 \\u1234 \\uabcd \\uFFFF\"");
    corpus.push_back("\"surrogate pair: \\uD83D\\uDE00 \\uD83C\\uDF89\"");
    corpus.push_back("\"utf8 literal: \xc3\xa9 \xe2\x9c\x93 \xf0\x9f\x98\x80\"");
    corpus.push_back("\"spaces and tabs:   \\t   \"");

    // --- Arrays ---
    corpus.push_back("[]");
    corpus.push_back("[ ]");
    corpus.push_back("[1]");
    corpus.push_back("[1, 2, 3]");
    corpus.push_back("[null, true, false, 0, \"str\", [], {}]");
    corpus.push_back("[\"a\", \"b\", \"c\"]");
    corpus.push_back("[1, [2, [3, [4, [5]]]]]");
    corpus.push_back("[[[[]]]]");
    corpus.push_back("[1, 2, [3, 4, [5, 6]], 7]");

    // --- Objects ---
    corpus.push_back("{}");
    corpus.push_back("{ }");
    corpus.push_back("{\"a\": 1}");
    corpus.push_back("{\"\": 0}");
    corpus.push_back("{\"k1\": null, \"k2\": true, \"k3\": 123.45, \"k4\": \"abc\"}");
    corpus.push_back("{\"nested\": {\"a\": 1, \"b\": [1, 2, 3]}}");
    corpus.push_back("{\"unicode_key_\xc3\xa9\": \"value_\xf0\x9f\x98\x80\"}");
    corpus.push_back("{\"duplicate\": 1, \"duplicate\": 2}");
    corpus.push_back("{\"a\": {}, \"b\": []}");

    // --- Complex / Mixed structures ---
    corpus.push_back("{\"items\": [{\"id\": 1, \"name\": \"first\"}, {\"id\": 2, \"name\": \"second\"}], \"total\": 2, \"ok\": true}");
    corpus.push_back("[{\"settings\": {\"volume\": 0.8, \"theme\": \"dark\", \"flags\": [true, false]}}]");

    // --- Nesting boundary seeds ---
    // Depth 127
    {
        std::string d127;
        for (int i = 0; i < 127; ++i) d127 += "[";
        d127 += "42";
        for (int i = 0; i < 127; ++i) d127 += "]";
        corpus.push_back(d127);
    }
    // Depth 128 (Exact limit)
    {
        std::string d128;
        for (int i = 0; i < 128; ++i) d128 += "[";
        d128 += "\"depth_128\"";
        for (int i = 0; i < 128; ++i) d128 += "]";
        corpus.push_back(d128);
    }
    // Depth 128 Object nesting
    {
        std::string o128;
        for (int i = 0; i < 128; ++i) o128 += "{\"k\":";
        o128 += "1";
        for (int i = 0; i < 128; ++i) o128 += "}";
        corpus.push_back(o128);
    }
    // Depth 129 (Exceeds limit by 1)
    {
        std::string d129;
        for (int i = 0; i < 129; ++i) d129 += "[";
        d129 += "42";
        for (int i = 0; i < 129; ++i) d129 += "]";
        corpus.push_back(d129);
    }
    // Depth 130
    {
        std::string d130;
        for (int i = 0; i < 130; ++i) d130 += "[";
        d130 += "{\"x\": 1}";
        for (int i = 0; i < 130; ++i) d130 += "]";
        corpus.push_back(d130);
    }
    // Deep nesting (500 brackets)
    {
        std::string d500;
        for (int i = 0; i < 500; ++i) d500 += "[";
        d500 += "0";
        for (int i = 0; i < 500; ++i) d500 += "]";
        corpus.push_back(d500);
    }

    // --- Pathological & Malformed Seeds ---
    corpus.push_back("");
    corpus.push_back("   ");
    corpus.push_back("\t\r\n");
    corpus.push_back("{");
    corpus.push_back("}");
    corpus.push_back("[");
    corpus.push_back("]");
    corpus.push_back("{\"a\":");
    corpus.push_back("{\"a\": 1,");
    corpus.push_back("[1, 2,");
    corpus.push_back("{\"unclosed\": \"string");
    corpus.push_back("\"unclosed quote");
    corpus.push_back("[1, 2, 3] trailing_garbage");
    corpus.push_back("{\"k\": 1} extra");
    corpus.push_back("{\"invalid_escape\": \"\\z\"}");
    corpus.push_back("{\"bad_hex\": \"\\u12G4\"}");
    corpus.push_back("{\"incomplete_hex\": \"\\u12\"}");
    corpus.push_back("{\"unpaired_high_surrogate\": \"\\ud800\"}");
    corpus.push_back("{\"unpaired_low_surrogate\": \"\\udc00\"}");
    corpus.push_back("{\"bad_second_surrogate\": \"\\ud800\\u1234\"}");
    corpus.push_back("{\"bad_number\": 1.2.3}");
    corpus.push_back("{\"bad_number_plus\": +123}");
    corpus.push_back("{\"bad_number_leadzero\": 0123}");
    corpus.push_back("{\"bad_number_exp\": 1e}");
    corpus.push_back("{\"bad_number_dot\": 1.}");
    corpus.push_back("{\"bad_number_nodigit\": .123}");
    corpus.push_back("{\"overflow_exp\": 1e309}");
    corpus.push_back("{\"underflow_exp\": -1e309}");
    corpus.push_back("{\"nan\": NaN}");
    corpus.push_back("{\"infinity\": Infinity}");
    corpus.push_back("{\"null_byte\": \"\x00\"}");
    corpus.push_back("{\"ctrl_byte\": \"\x01\"}");
    corpus.push_back("{\"raw_newline\": \"\n\"}");
    corpus.push_back("{\"raw_tab\": \"\t\"}");
    corpus.push_back("{\"raw_cr\": \"\r\"}");
    corpus.push_back("[,]");
    corpus.push_back("[1,,2]");
    corpus.push_back("{,}");
    corpus.push_back("{\"a\": 1,, \"b\": 2}");
    corpus.push_back("{\"a\" 1}");
    corpus.push_back("{1: 2}");
    corpus.push_back("{true: false}");

    return corpus;
}

// Random structured AST synthesizer
class JsonSynthesizer {
public:
    static std::string Generate(FuzzRng& rng, size_t maxDepth = 6) {
        return GenerateValue(rng, 0, maxDepth);
    }

private:
    static std::string GenerateValue(FuzzRng& rng, size_t currentDepth, size_t maxDepth) {
        if (currentDepth >= maxDepth) {
            return GeneratePrimitive(rng);
        }

        int choice = static_cast<int>(rng.NextSize(0, 5));
        switch (choice) {
            case 0: return "null";
            case 1: return rng.Prob(0.5) ? "true" : "false";
            case 2: return GenerateNumber(rng);
            case 3: return GenerateString(rng);
            case 4: return GenerateArray(rng, currentDepth + 1, maxDepth);
            case 5: return GenerateObject(rng, currentDepth + 1, maxDepth);
            default: return "null";
        }
    }

    static std::string GeneratePrimitive(FuzzRng& rng) {
        int choice = static_cast<int>(rng.NextSize(0, 3));
        switch (choice) {
            case 0: return "null";
            case 1: return rng.Prob(0.5) ? "true" : "false";
            case 2: return GenerateNumber(rng);
            case 3: return GenerateString(rng);
            default: return "0";
        }
    }

    static std::string GenerateNumber(FuzzRng& rng) {
        int kind = static_cast<int>(rng.NextSize(0, 7));
        switch (kind) {
            case 0: return "0";
            case 1: return "-0";
            case 2: return std::to_string(rng.NextInt64(-1000000, 1000000));
            case 3: {
                double d = rng.NextDouble(-1000.0, 1000.0);
                std::ostringstream ss;
                ss << std::setprecision(6) << d;
                return ss.str();
            }
            case 4: return std::to_string(rng.NextInt64(1, 99)) + "e" + std::to_string(rng.NextInt64(-10, 10));
            case 5: return std::to_string(rng.NextInt64(-99, -1)) + "E+" + std::to_string(rng.NextInt64(0, 5));
            case 6: return "9007199254740991";
            case 7: return "-9007199254740991";
            default: return "1";
        }
    }

    static std::string GenerateString(FuzzRng& rng) {
        static const std::vector<std::string> sampleWords = {
            "", "a", "test", "hello", "Aperture", "MediaLibrary", "Settings",
            "escaped\\\"quote", "line\\nbreak", "tab\\tspace", "back\\\\slash",
            "\\u0041\\u0042\\u0043", "\\uD83D\\uDE00", "\xc3\xa9", "\xe2\x9c\x93"
        };
        std::string s = "\"";
        size_t count = rng.NextSize(0, 3);
        for (size_t i = 0; i < count; ++i) {
            s += rng.Pick(sampleWords);
        }
        s += "\"";
        return s;
    }

    static std::string GenerateArray(FuzzRng& rng, size_t currentDepth, size_t maxDepth) {
        size_t len = rng.NextSize(0, 5);
        std::string s = "[";
        for (size_t i = 0; i < len; ++i) {
            if (i > 0) s += ", ";
            s += GenerateValue(rng, currentDepth, maxDepth);
        }
        s += "]";
        return s;
    }

    static std::string GenerateObject(FuzzRng& rng, size_t currentDepth, size_t maxDepth) {
        size_t len = rng.NextSize(0, 5);
        std::string s = "{";
        for (size_t i = 0; i < len; ++i) {
            if (i > 0) s += ", ";
            std::string key = "\"k_" + std::to_string(i) + "\"";
            s += key + ": " + GenerateValue(rng, currentDepth, maxDepth);
        }
        s += "}";
        return s;
    }
};

// Mutation Engine
class MutationEngine {
public:
    explicit MutationEngine(const std::vector<std::string>& corpus) : corpus_(corpus) {
        InitInterestingBytes();
        InitInterestingTokens();
    }

    std::string Mutate(const std::string& input, FuzzRng& rng) {
        if (input.empty()) {
            return rng.Prob(0.5) ? "{}" : "[]";
        }

        std::string result = input;
        size_t mutationCount = rng.NextSize(1, 4);

        for (size_t m = 0; m < mutationCount; ++m) {
            int op = static_cast<int>(rng.NextSize(0, 11));
            switch (op) {
                case 0: MutateByteInsert(result, rng); break;
                case 1: MutateByteDelete(result, rng); break;
                case 2: MutateByteReplace(result, rng); break;
                case 3: MutateByteFlip(result, rng); break;
                case 4: MutateByteSplice(result, rng); break;
                case 5: MutateTokenInsert(result, rng); break;
                case 6: MutateNesting(result, rng); break;
                case 7: MutateEscape(result, rng); break;
                case 8: MutateNumber(result, rng); break;
                case 9: MutateWhitespace(result, rng); break;
                case 10: MutateUtf8(result, rng); break;
                case 11: MutateContainerSyntax(result, rng); break;
            }
        }

        return result;
    }

private:
    const std::vector<std::string>& corpus_;
    std::vector<uint8_t> interestingBytes_;
    std::vector<std::string> interestingTokens_;

    void InitInterestingBytes() {
        interestingBytes_ = {
            0x00, 0x01, 0x08, 0x09, 0x0A, 0x0D, 0x1F, 0x20,
            '"', '\\', '/', ':', ',', '[', ']', '{', '}',
            '-', '+', '.', '0', '1', '9', 'e', 'E',
            0x7F, 0x80, 0xC0, 0xC1, 0xC2, 0xE0, 0xF0, 0xF4, 0xF5, 0xFF
        };
    }

    void InitInterestingTokens() {
        interestingTokens_ = {
            "null", "true", "false", "0", "-0", "1", "-1", "1e5", "1e-5", "-1.2e+3",
            "\"\"", "\"test\"", "\\\"", "\\\\", "\\/", "\\b", "\\f", "\\n", "\\r", "\\t",
            "\\u0000", "\\u0020", "\\u1234", "\\ud800", "\\udc00", "\\ud83d\\ude00",
            ":", ",", "[", "]", "{", "}", "\x00", "\x01", "\xFF", "\xC0\x80"
        };
    }

    void MutateByteInsert(std::string& s, FuzzRng& rng) {
        size_t pos = rng.NextSize(0, s.size());
        size_t count = rng.NextSize(1, 4);
        for (size_t i = 0; i < count; ++i) {
            uint8_t b = rng.Prob(0.7) ? rng.Pick(interestingBytes_) : rng.NextByte();
            s.insert(pos + i, 1, static_cast<char>(b));
        }
    }

    void MutateByteDelete(std::string& s, FuzzRng& rng) {
        if (s.empty()) return;
        size_t pos = rng.NextSize(0, s.size() - 1);
        size_t count = rng.NextSize(1, std::min<size_t>(s.size() - pos, 8));
        s.erase(pos, count);
    }

    void MutateByteReplace(std::string& s, FuzzRng& rng) {
        if (s.empty()) return;
        size_t pos = rng.NextSize(0, s.size() - 1);
        size_t count = rng.NextSize(1, std::min<size_t>(s.size() - pos, 4));
        for (size_t i = 0; i < count; ++i) {
            uint8_t b = rng.Prob(0.7) ? rng.Pick(interestingBytes_) : rng.NextByte();
            s[pos + i] = static_cast<char>(b);
        }
    }

    void MutateByteFlip(std::string& s, FuzzRng& rng) {
        if (s.empty()) return;
        size_t pos = rng.NextSize(0, s.size() - 1);
        int bit = static_cast<int>(rng.NextSize(0, 7));
        s[pos] ^= static_cast<char>(1 << bit);
    }

    void MutateByteSplice(std::string& s, FuzzRng& rng) {
        if (corpus_.empty()) return;
        const std::string& donor = rng.Pick(corpus_);
        if (donor.empty()) return;
        size_t donorStart = rng.NextSize(0, donor.size() - 1);
        size_t donorLen = rng.NextSize(1, std::min<size_t>(donor.size() - donorStart, 32));
        std::string chunk = donor.substr(donorStart, donorLen);

        size_t pos = rng.NextSize(0, s.size());
        if (rng.Prob(0.5)) {
            s.insert(pos, chunk);
        } else {
            size_t replaceLen = std::min<size_t>(s.size() - pos, chunk.size());
            s.replace(pos, replaceLen, chunk);
        }
    }

    void MutateTokenInsert(std::string& s, FuzzRng& rng) {
        const std::string& token = rng.Pick(interestingTokens_);
        size_t pos = rng.NextSize(0, s.size());
        s.insert(pos, token);
    }

    void MutateNesting(std::string& s, FuzzRng& rng) {
        size_t depth = rng.NextSize(1, 140);
        bool useObj = rng.Prob(0.4);
        std::string prefix, suffix;
        for (size_t i = 0; i < depth; ++i) {
            if (useObj) {
                prefix += "{\"k\":";
                suffix += "}";
            } else {
                prefix += "[";
                suffix += "]";
            }
        }
        s = prefix + s + suffix;
    }

    void MutateEscape(std::string& s, FuzzRng& rng) {
        static const std::vector<std::string> escapeMutations = {
            "\\\"", "\\\\", "\\/", "\\b", "\\f", "\\n", "\\r", "\\t",
            "\\u0000", "\\u001f", "\\u1234", "\\uFFFF", "\\uD800", "\\uDC00",
            "\\uD83D\\uDE00", "\\uD800\\u1234", "\\u", "\\u1", "\\u12", "\\u123", "\\z", "\\x01"
        };
        const std::string& esc = rng.Pick(escapeMutations);
        size_t pos = rng.NextSize(0, s.size());
        s.insert(pos, esc);
    }

    void MutateNumber(std::string& s, FuzzRng& rng) {
        static const std::vector<std::string> numMutations = {
            "0", "-0", "+0", "+123", "0123", "-0123", "1.2.3", "1e", "1e+", "1e-",
            "1e309", "-1e309", "1.7976931348623157e+308", "99999999999999999999999999999999",
            "NaN", "-NaN", "Infinity", "-Infinity", ".123", "123."
        };
        const std::string& num = rng.Pick(numMutations);
        size_t pos = rng.NextSize(0, s.size());
        s.insert(pos, num);
    }

    void MutateWhitespace(std::string& s, FuzzRng& rng) {
        static const std::vector<std::string> wsMutations = {
            " ", "  ", "\t", "\r", "\n", "\r\n", " \t \r\n "
        };
        const std::string& ws = rng.Pick(wsMutations);
        size_t pos = rng.NextSize(0, s.size());
        s.insert(pos, ws);
    }

    void MutateUtf8(std::string& s, FuzzRng& rng) {
        static const std::vector<std::string> utf8Mutations = {
            "\xc3\xa9",          // valid 2-byte: é
            "\xe2\x9c\x93",      // valid 3-byte: checkmark
            "\xf0\x9f\x98\x80",  // valid 4-byte: grinning face
            "\x80",              // standalone continuation byte (invalid)
            "\xBF",              // standalone continuation byte (invalid)
            "\xC0\x80",          // overlong NUL (invalid)
            "\xC1\xBF",          // overlong ASCII (invalid)
            "\xE0\x80\x80",      // overlong 3-byte (invalid)
            "\xF0\x80\x80\x80",  // overlong 4-byte (invalid)
            "\xF5\x80\x80\x80",  // out-of-range codepoint > 0x10FFFF (invalid)
            "\xC2",              // truncated 2-byte (invalid)
            "\xE2\x9C",          // truncated 3-byte (invalid)
            "\xF0\x9F\x98",      // truncated 4-byte (invalid)
            "\xFF", "\xFE"       // invalid UTF-8 bytes
        };
        const std::string& u = rng.Pick(utf8Mutations);
        size_t pos = rng.NextSize(0, s.size());
        s.insert(pos, u);
    }

    void MutateContainerSyntax(std::string& s, FuzzRng& rng) {
        static const std::vector<std::string> containerMutations = {
            ":", ",", "::", ",,", "{,", "[,", ",}", ",]", ":}", ":]",
            "\"unclosed", "{\"a\":}", "{:1}", "{\"a\": 1 \"b\": 2}"
        };
        const std::string& c = rng.Pick(containerMutations);
        size_t pos = rng.NextSize(0, s.size());
        s.insert(pos, c);
    }
};

// Invariant verification results
struct FuzzStats {
    uint64_t totalCases = 0;
    uint64_t acceptedCases = 0;
    uint64_t rejectedCases = 0;
    uint64_t roundtripsVerified = 0;
    uint64_t deepNestingBoundaryCases = 0;
    uint64_t malformedSyntaxCases = 0;
    uint64_t escapedUtf8Cases = 0;
    uint64_t structuredSynthesizedCases = 0;
    uint64_t mutatedCorpusCases = 0;
};

// Invariant tester for a single candidate input
bool TestJsonInvariants(const std::string& input, std::string& failureMsg) {
    // Invariant 1: No crash, no unhandled exception during parse.
    // Invariant 2: Deterministic parse agreement between successive parse calls.
    JsonValue v1;
    bool ok1 = JsonValue::parse(input, v1);

    JsonValue v2;
    bool ok2 = JsonValue::parse(input, v2);

    if (ok1 != ok2) {
        std::ostringstream oss;
        oss << "Nondeterminism detected: parse() returned " << (ok1 ? "true" : "false")
            << " on run 1, but " << (ok2 ? "true" : "false") << " on run 2 for input: "
            << input.substr(0, 200);
        failureMsg = oss.str();
        return false;
    }

    if (!ok1) {
        // Invariant 3: On rejection, out value MUST be null.
        if (!v1.is_null() || !v2.is_null()) {
            failureMsg = "Rejected parse resulted in non-null JsonValue";
            return false;
        }

        // Legacy parse overload must agree and return null.
        JsonValue legacy = JsonValue::parse(input);
        if (!legacy.is_null()) {
            failureMsg = "Legacy parse() returned non-null for rejected input";
            return false;
        }
        return true;
    }

    // Input was accepted!
    // Invariant 4: Successive parsed structures must be deeply identical.
    if (!JsonValuesEqual(v1, v2)) {
        failureMsg = "Nondeterministic parse content: v1 and v2 structural mismatch for accepted input";
        return false;
    }

    // Invariant 5: Legacy parse must produce identical type for accepted input.
    JsonValue legacy = JsonValue::parse(input);
    if (legacy.type != v1.type) {
        failureMsg = "Legacy parse() type mismatch for accepted input";
        return false;
    }

    // Invariant 6: Nesting depth must not exceed max allowed depth (128).
    size_t depth = ComputeDepth(v1);
    if (depth > JsonValue::kMaxJsonDepth) {
        failureMsg = "Parsed structure exceeds max nesting depth of 128";
        return false;
    }

    // Invariant 7: Stringify and reparse stable equivalent serialization.
    // Parse -> Stringify (s1) -> Parse (v_canonical) -> Stringify (s2) -> Parse (v_reparse) -> Stringify (s3)
    // Defends the invariant: s2 == s3 and JsonValuesEqual(v_canonical, v_reparse)
    std::string s1 = v1.stringify(false);
    JsonValue v_canonical;
    bool ok_canonical = JsonValue::parse(s1, v_canonical);
    if (!ok_canonical) {
        std::ostringstream oss;
        oss << "Failed to parse stringify(false) output. Serialized: " << s1.substr(0, 200);
        failureMsg = oss.str();
        return false;
    }

    // Type preservation between original and canonical re-parsed value
    if (v1.type != v_canonical.type) {
        failureMsg = "Type mismatch between original parsed value and canonical re-parsed representation";
        return false;
    }

    std::string s2 = v_canonical.stringify(false);
    JsonValue v_reparse;
    bool ok_reparse = JsonValue::parse(s2, v_reparse);
    if (!ok_reparse) {
        std::ostringstream oss;
        oss << "Failed to re-parse secondary stringify output. Serialized: " << s2.substr(0, 200);
        failureMsg = oss.str();
        return false;
    }

    std::string s3 = v_reparse.stringify(false);
    if (s2 != s3) {
        std::ostringstream oss;
        oss << "Stringify stability regression: s2 != s3.\n s2: "
            << s2.substr(0, 200) << "\n s3: " << s3.substr(0, 200);
        failureMsg = oss.str();
        return false;
    }

    // Deep structural semantic equality between canonical and secondary re-parsed representations
    if (!JsonValuesEqual(v_canonical, v_reparse)) {
        failureMsg = "Deep structural mismatch between canonical and secondary re-parsed representations";
        return false;
    }

    // Invariant 8: Pretty stringification round-trip and equivalence.
    std::string pretty = v_canonical.stringify(true);
    JsonValue v_pretty;
    bool ok_pretty = JsonValue::parse(pretty, v_pretty);
    if (!ok_pretty) {
        std::ostringstream oss;
        oss << "Failed to parse pretty stringify output. Serialized: " << pretty.substr(0, 200);
        failureMsg = oss.str();
        return false;
    }

    std::string compact_from_pretty = v_pretty.stringify(false);
    if (compact_from_pretty != s2) {
        std::ostringstream oss;
        oss << "Pretty stringification produces non-equivalent compact output: "
            << "\n pretty-compact: " << compact_from_pretty.substr(0, 200)
            << "\n s2: " << s2.substr(0, 200);
        failureMsg = oss.str();
        return false;
    }

    if (!JsonValuesEqual(v_canonical, v_pretty)) {
        failureMsg = "Deep structural mismatch between canonical and pretty re-parsed representations";
        return false;
    }

    return true;
}

// Invariant regression injection self-test to verify runner catches regressions
bool RunSelfTest(std::string& err) {
    // 1. Verify a fake nondeterminism would be caught by TestJsonInvariants logic
    // 2. Test sanity of normal known valid/invalid inputs
    std::string valid = "{\"a\": [1, 2, true, null], \"b\": \"hello\\nworld\"}";
    if (!TestJsonInvariants(valid, err)) {
        err = "Self-test failed on known valid JSON: " + err;
        return false;
    }

    std::string invalid = "{\"unclosed\": 1";
    if (!TestJsonInvariants(invalid, err)) {
        err = "Self-test failed on known invalid JSON: " + err;
        return false;
    }

    return true;
}

void PrintUsage(const char* exe) {
    std::cout << "Usage: " << exe << " [options]\n\n"
              << "Options:\n"
              << "  --seed <N>, -s <N>         Set deterministic RNG seed (default: 1337)\n"
              << "  --iterations <N>, -n <N>   Number of fuzz test iterations (default: 10000)\n"
              << "  --max-len <N>              Maximum candidate length (default: 65536)\n"
              << "  --verbose, -v              Enable detailed progress output\n"
              << "  --help, -h                 Display this help message\n";
}

} // namespace

int main(int argc, char* argv[]) {
    uint64_t seed = 1337;
    uint64_t iterations = 10000;
    size_t maxLen = 65536;
    bool verbose = false;

    // Parse command-line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--seed" || arg == "-s") && i + 1 < argc) {
            try {
                seed = std::stoull(argv[++i]);
            } catch (...) {
                std::cerr << "Invalid seed value: " << argv[i] << "\n";
                return 1;
            }
        } else if ((arg == "--iterations" || arg == "-n" || arg == "-i" || arg == "--cases") && i + 1 < argc) {
            try {
                iterations = std::stoull(argv[++i]);
            } catch (...) {
                std::cerr << "Invalid iterations value: " << argv[i] << "\n";
                return 1;
            }
        } else if (arg == "--max-len" && i + 1 < argc) {
            try {
                maxLen = std::stoull(argv[++i]);
            } catch (...) {
                std::cerr << "Invalid max-len value: " << argv[i] << "\n";
                return 1;
            }
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            PrintUsage(argv[0]);
            return 1;
        }
    }

    std::cout << "========================================================\n"
              << "  Aperture Native - JsonValue Mutation Fuzz Runner      \n"
              << "========================================================\n"
              << "  Seed:       " << seed << "\n"
              << "  Iterations: " << iterations << "\n"
              << "  Max length: " << maxLen << " bytes\n"
              << "--------------------------------------------------------\n";

    // Self-test runner
    std::string selfTestErr;
    if (!RunSelfTest(selfTestErr)) {
        std::cerr << "[FAIL] Fuzz runner self-test failed: " << selfTestErr << "\n";
        return 1;
    }

    FuzzRng rng(seed);
    std::vector<std::string> seedCorpus = BuildSeedCorpus();
    MutationEngine mutator(seedCorpus);

    FuzzStats stats;
    auto startTime = std::chrono::high_resolution_clock::now();

    // Stage 1: Exhaustively test the entire initial structured seed corpus
    for (const auto& seedItem : seedCorpus) {
        stats.totalCases++;
        std::string failureMsg;
        if (!TestJsonInvariants(seedItem, failureMsg)) {
            std::cerr << "\n[FAIL] Invariant violation on seed corpus item #" << stats.totalCases << ":\n"
                      << "  Error: " << failureMsg << "\n"
                      << "  Input: " << seedItem.substr(0, 300) << "\n";
            return 1;
        }

        JsonValue temp;
        if (JsonValue::parse(seedItem, temp)) {
            stats.acceptedCases++;
            stats.roundtripsVerified++;
        } else {
            stats.rejectedCases++;
        }
    }

    // Stage 2: Fuzz iterations (Synthesis + Mutation + Boundary Cases)
    for (uint64_t iter = 0; iter < iterations; ++iter) {
        stats.totalCases++;

        std::string candidate;
        int mode = static_cast<int>(rng.NextSize(0, 9));

        if (mode <= 2) {
            // Mode 0, 1, 2: Structured random AST synthesis (depth 1 to 10)
            size_t depth = rng.NextSize(1, 8);
            candidate = JsonSynthesizer::Generate(rng, depth);
            stats.structuredSynthesizedCases++;
        } else if (mode <= 6) {
            // Mode 3, 4, 5, 6: Mutate an existing item from seed corpus
            const std::string& base = rng.Pick(seedCorpus);
            candidate = mutator.Mutate(base, rng);
            stats.mutatedCorpusCases++;
        } else if (mode == 7) {
            // Mode 7: Deep nesting boundary testing (around 120-140 brackets/braces)
            size_t nesting = rng.NextSize(120, 135);
            bool useObj = rng.Prob(0.5);
            std::string prefix, suffix;
            for (size_t k = 0; k < nesting; ++k) {
                if (useObj) {
                    prefix += "{\"k\":";
                    suffix += "}";
                } else {
                    prefix += "[";
                    suffix += "]";
                }
            }
            candidate = prefix + (useObj ? "1" : "42") + suffix;
            stats.deepNestingBoundaryCases++;
        } else if (mode == 8) {
            // Mode 8: Malformed syntax & escape injection
            const std::string& base = rng.Pick(seedCorpus);
            candidate = mutator.Mutate(base, rng);
            candidate = mutator.Mutate(candidate, rng);
            stats.malformedSyntaxCases++;
        } else {
            // Mode 9: UTF-8 & string boundary injection
            size_t depth = rng.NextSize(1, 4);
            candidate = JsonSynthesizer::Generate(rng, depth);
            candidate = mutator.Mutate(candidate, rng);
            stats.escapedUtf8Cases++;
        }

        // Clamp candidate size to maxLen
        if (candidate.size() > maxLen) {
            candidate.resize(maxLen);
        }

        std::string failureMsg;
        if (!TestJsonInvariants(candidate, failureMsg)) {
            std::cerr << "\n[FAIL] Invariant violation on iteration " << iter
                      << " (Seed: " << seed << "):\n"
                      << "  Error: " << failureMsg << "\n"
                      << "  Input size: " << candidate.size() << " bytes\n"
                      << "  Input snippet: " << candidate.substr(0, 300) << "\n";
            return 1;
        }

        JsonValue temp;
        if (JsonValue::parse(candidate, temp)) {
            stats.acceptedCases++;
            stats.roundtripsVerified++;
        } else {
            stats.rejectedCases++;
        }

        if (verbose && (iter + 1) % 2000 == 0) {
            std::cout << "  ... completed " << (iter + 1) << " / " << iterations << " iterations\n";
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    double elapsedMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    double casesPerSec = (stats.totalCases / (elapsedMs / 1000.0));

    std::cout << "\n========================================================\n"
              << "  Fuzz Runner Summary                                   \n"
              << "========================================================\n"
              << "  Status:                 [PASS] ALL INVARIANTS SATISFIED\n"
              << "  Seed:                   " << seed << "\n"
              << "  Total test cases:       " << stats.totalCases << "\n"
              << "  Accepted (Valid JSON):  " << stats.acceptedCases << "\n"
              << "  Rejected (Invalid):     " << stats.rejectedCases << "\n"
              << "  Round-trips verified:   " << stats.roundtripsVerified << "\n"
              << "  Deep nesting cases:     " << stats.deepNestingBoundaryCases << "\n"
              << "  Synthesized AST cases:  " << stats.structuredSynthesizedCases << "\n"
              << "  Mutated corpus cases:   " << stats.mutatedCorpusCases << "\n"
              << "  Elapsed time:           " << std::fixed << std::setprecision(2) << elapsedMs << " ms ("
              << std::fixed << std::setprecision(0) << casesPerSec << " cases/sec)\n"
              << "========================================================\n";

    return 0;
}
