#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Json.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include <cerrno>
#include <cstdlib>

namespace Aperture::Test {

TEST_CASE(JsonLimits, SerializationFormattingAndMoveInsertion) {
    const std::string compact = "{\"a\":[1.25,true,null,\"line\\n\\t\\u0001\"],\"b\":{\"empty\":[],\"object\":{}}}";
    JsonValue value;
    TEST_ASSERT(JsonValue::parse(compact, value));
    TEST_ASSERT_EQ(value.stringify(), compact);
    const std::string pretty =
        "{\n  \"a\": [\n    1.25,\n    true,\n    null,\n    \"line\\n\\t\\u0001\"\n  ],\n"
        "  \"b\": {\n    \"empty\": [],\n    \"object\": {}\n  }\n}";
    TEST_ASSERT_EQ(value.stringify(true), pretty);
    JsonValue roundTrip;
    TEST_ASSERT(JsonValue::parse(pretty, roundTrip));
    TEST_ASSERT_EQ(roundTrip.stringify(), compact);

    JsonValue array = JsonValue::Array();
    array.push_back(value);
    array.push_back(std::move(roundTrip));
    TEST_ASSERT_EQ(array[0].stringify(), compact);
    TEST_ASSERT_EQ(array[1].stringify(), compact);
    TEST_ASSERT_EQ(value.stringify(), compact); // lvalue insertion still copies
    TEST_ASSERT_EQ(JsonValue(std::numeric_limits<double>::infinity()).stringify(), std::string("null"));
}

TEST_CASE(JsonLimits, AppendSerializationCompatibility) {
    const std::string utf8 = "café/雪";
    std::string escaped;
    escaped.push_back('\0');
    escaped.push_back('\x01');
    escaped.push_back('\x1f');
    escaped += "\b\f\n\r\t\\\"";
    escaped += utf8;

    JsonValue value = JsonValue::Object();
    value["z"] = JsonValue(1.23456789);
    value["i"] = JsonValue(-42);
    value["s"] = JsonValue(escaped);
    value["exp"] = JsonValue(1.0e20);
    const std::string expected =
        "{\"exp\":1e+20,\"i\":-42,\"s\":\"\\u0000\\u0001\\u001f\\b\\f\\n\\r\\t\\\\\\\"café/雪\",\"z\":1.23457}";

    TEST_ASSERT_EQ(value.stringify(), expected);
    std::string appended = "prefix:";
    value.append_to(appended);
    TEST_ASSERT_EQ(appended, "prefix:" + expected);

    const std::string pretty = value.stringify(true, 1);
    std::string prettyAppended = "prefix:";
    value.append_to(prettyAppended, true, 1);
    TEST_ASSERT_EQ(prettyAppended, "prefix:" + pretty);
    JsonValue parsed;
    TEST_ASSERT(JsonValue::parse(expected, parsed));
    TEST_ASSERT_EQ(parsed.get("s").as_string(), escaped);
}

TEST_CASE(JsonLimits, ObjectKeyOrderAndDuplicateCompatibility) {
    JsonValue parsed;
    TEST_ASSERT(JsonValue::parse("{\"z\":1,\"a\":2,\"m\":3,\"z\":{\"nested\":true},\"a\":4}", parsed));
    TEST_ASSERT_EQ(parsed.stringify(), std::string("{\"a\":4,\"m\":3,\"z\":{\"nested\":true}}"));
    TEST_ASSERT(JsonValue::parse(parsed.stringify(), parsed));
    TEST_ASSERT_EQ(parsed.get("z").get("nested").as_bool(), true);
}

TEST_CASE(JsonLimits, NestedArrayGrowthPreservesOwnedValues) {
    // Cross many growth boundaries with every payload type, long strings,
    // nested containers and duplicate keys. Input lifetime must not matter.
    JsonValue parsed;
    {
        std::string input = "[";
        for (int i = 0; i < 300; ++i) {
            if (i) input += ',';
            input += "{\"id\":-1,\"id\":" + std::to_string(i) +
                ",\"values\":[null,true,false,1.25,[],{},[\"" + std::string(128, 'x') +
                "\\u96ea\",{\"plugin\":[1,2,3,4,5,6,7,8,9]}]]}";
        }
        input += ']';
        TEST_ASSERT(JsonValue::parse(input, parsed));
    }
    TEST_ASSERT_EQ(parsed.as_array().size(), size_t(300));
    const std::string expectedText = std::string(128, 'x') + "雪";
    for (int i = 0; i < 300; ++i) {
        TEST_ASSERT_EQ(parsed[i].get("id").as_int(), i);
        const auto& values = parsed[i].get("values");
        TEST_ASSERT(values[0].is_null());
        TEST_ASSERT(values[1].as_bool());
        TEST_ASSERT(!values[2].as_bool(true));
        TEST_ASSERT_EQ(values[3].as_number(), 1.25);
        TEST_ASSERT(values[4].is_array() && values[4].as_array().empty());
        TEST_ASSERT(values[5].is_object() && values[5].as_object().empty());
        TEST_ASSERT_EQ(values[6][0].as_string(), expectedText);
        TEST_ASSERT_EQ(values[6][1].get("plugin").as_array().size(), size_t(9));
        TEST_ASSERT_EQ(values[6][1].get("plugin")[8].as_int(), 9);
    }
    JsonValue copied = parsed;
    copied[0]["values"][6][0] = "changed";
    TEST_ASSERT_EQ(parsed[0].get("values")[6][0].as_string(), expectedText);
    JsonValue roundTrip;
    TEST_ASSERT(JsonValue::parse(parsed.stringify(), roundTrip));
    TEST_ASSERT(roundTrip == parsed);
    TEST_ASSERT(!JsonValue::parse(parsed.stringify() + " trailing", parsed));
    TEST_ASSERT(parsed.is_null());
}

TEST_CASE(JsonLimits, SpanStringsAndEscapeBoundaries) {
    const std::string text = std::string(4096, 'x') + "雪" + std::string("\n\t\0", 3) + std::string(4096, 'y');
    JsonValue parsed;
    TEST_ASSERT(JsonValue::parse(JsonValue(text).stringify(), parsed));
    TEST_ASSERT_EQ(parsed.as_string(), text);
    TEST_ASSERT(JsonValue::parse("\"\\uD83D\\uDE00\\u0000end\"", parsed));
    TEST_ASSERT_EQ(parsed.as_string(), std::string("\xF0\x9F\x98\x80\0end", 8));
    for (const std::string input : {std::string("\"unterminated"), std::string("\"abc\\"),
            std::string("\"a\x01" "b\""), std::string("\"\\q\""), std::string("\"\\uD800\""), std::string("\"\\uDC00\"")}) {
        TEST_ASSERT(!JsonValue::parse(input, parsed));
        TEST_ASSERT(parsed.is_null());
    }
    // Bound decoded bytes, including multi-byte escapes at the exact limit.
    const std::string prefix(JsonValue::kMaxJsonStringBytes - 3, 'x');
    TEST_ASSERT(JsonValue::parse("\"" + prefix + "\\u20ac\"", parsed));
    TEST_ASSERT_EQ(parsed.as_string_ref().size(), JsonValue::kMaxJsonStringBytes);
    TEST_ASSERT(!JsonValue::parse("\"" + prefix + "x\\u20ac\"", parsed));
    TEST_ASSERT(!JsonValue::parse("\"" + prefix + "xxxx\"", parsed));
}

TEST_CASE(JsonLimits, DirectNumberParsingCompatibility) {
    JsonValue parsed;
    for (const char* text : {"0", "-0", "1.25", "-123456789.012345", "9007199254740993", "18446744073709551615",
            "1e20", "1E+30", "-2e-30", "1.7976931348623157e308", "2.2250738585072014e-308",
            "2.225073858507201e-308", "4.9406564584124654e-324", "1e-999", "1e999"}) {
        errno = 0;
        char* end = nullptr;
        const double expected = std::strtod(text, &end);
        const bool accepted = errno != ERANGE && std::isfinite(expected) && *end == '\0';
        TEST_ASSERT_EQ(JsonValue::parse(text, parsed), accepted);
        if (accepted) {
            TEST_ASSERT_EQ(parsed.as_number(), expected);
            TEST_ASSERT_EQ(std::signbit(parsed.as_number()), std::signbit(expected));
        }
    }
    for (const char* text : {"+1", "01", "-01", ".1", "1.", "1e", "1e+", "NaN", "Infinity", "1xyz", "0x1"}) {
        TEST_ASSERT(!JsonValue::parse(text, parsed));
        TEST_ASSERT(parsed.is_null());
    }
    TEST_ASSERT(JsonValue::parse("[42,1.25,-3e2]", parsed));
    TEST_ASSERT_EQ(parsed[0].as_int(), 42);
    TEST_ASSERT_EQ(parsed[1].as_number(), 1.25);
    TEST_ASSERT_EQ(parsed[2].as_number(), -300.0);
}

TEST_CASE(JsonLimits, MaxInputBytesRejection) {
    // Empty input
    JsonValue out1;
    TEST_ASSERT(!JsonValue::parse("", out1));
    TEST_ASSERT(out1.is_null());

    // Construct an input exceeding 64 MB by 1 byte
    // Instead of allocating 64MB every time, verify constant and boundary condition
    TEST_ASSERT_EQ(JsonValue::kMaxJsonInputBytes, (size_t)(64 * 1024 * 1024));
    TEST_ASSERT_EQ(JsonValue::kMaxJsonDepth, (size_t)128);
    TEST_ASSERT_EQ(JsonValue::kMaxJsonStringBytes, (size_t)(16 * 1024 * 1024));
    TEST_ASSERT_EQ(JsonValue::kMaxJsonContainerElements, (size_t)1000000);
}

TEST_CASE(JsonLimits, ArrayGrowthHonorsElementLimit) {
    std::string input = "[";
    input.reserve(2 * JsonValue::kMaxJsonContainerElements + 3);
    for (size_t i = 0; i < JsonValue::kMaxJsonContainerElements; ++i) {
        if (i) input += ',';
        input += '0';
    }
    input += ']';
    JsonValue parsed;
    TEST_ASSERT(JsonValue::parse(input, parsed));
    TEST_ASSERT_EQ(parsed.as_array().size(), JsonValue::kMaxJsonContainerElements);
    TEST_ASSERT_EQ(parsed[JsonValue::kMaxJsonContainerElements - 1].as_int(-1), 0);
    parsed = JsonValue();
    input.pop_back();
    input += ",0]";
    TEST_ASSERT(!JsonValue::parse(input, parsed));
    TEST_ASSERT(parsed.is_null());
}

TEST_CASE(JsonLimits, NestingDepthRejection) {
    // Construct nesting depth of exactly 128 levels (should succeed)
    std::string okNesting;
    for (int i = 0; i < 128; ++i) okNesting += "[";
    okNesting += "42";
    for (int i = 0; i < 128; ++i) okNesting += "]";

    JsonValue okVal;
    TEST_ASSERT(JsonValue::parse(okNesting, okVal));
    TEST_ASSERT(okVal.is_array());

    // Construct nesting depth of 129 levels (exceeds max depth 128 -> must fail)
    std::string badNesting;
    for (int i = 0; i < 129; ++i) badNesting += "[";
    badNesting += "42";
    for (int i = 0; i < 129; ++i) badNesting += "]";

    JsonValue badVal;
    TEST_ASSERT(!JsonValue::parse(badNesting, badVal));
    TEST_ASSERT(badVal.is_null());

    // Object nesting of 129 levels
    std::string badObjNesting;
    for (int i = 0; i < 129; ++i) badObjNesting += "{\"k\":";
    badObjNesting += "1";
    for (int i = 0; i < 129; ++i) badObjNesting += "}";

    JsonValue badObjVal;
    TEST_ASSERT(!JsonValue::parse(badObjNesting, badObjVal));
    TEST_ASSERT(badObjVal.is_null());
}

TEST_CASE(JsonLimits, DeeplyNestedFixtureRejection) {
    std::wstring fixturePath = GetExeDir() + L"\\tests\\fixtures\\deeply_nested.json";
    if (!FileExists(fixturePath)) {
        fixturePath = L"tests\\fixtures\\deeply_nested.json";
    }
    if (!FileExists(fixturePath)) {
        fixturePath = L"..\\tests\\fixtures\\deeply_nested.json";
    }
    if (!FileExists(fixturePath)) {
        fixturePath = L"..\\..\\tests\\fixtures\\deeply_nested.json";
    }

    std::string content;
    if (ReadTextFile(fixturePath, content).is_ok()) {
        JsonValue parsed;
        TEST_ASSERT(!JsonValue::parse(content, parsed));
        TEST_ASSERT(parsed.is_null());
    }
}

TEST_CASE(JsonLimits, MalformedJsonRobustness) {
    std::vector<std::string> malformedInputs = {
        "{\"unclosed\": \"string",
        "{\"unclosed_obj\": 1",
        "[1, 2, 3",
        "{\"trailing\": 1} extra_characters",
        "{\"invalid_escape\": \"\\z\"}",
        "{\"bad_hex\": \"\\u12G4\"}",
        "{\"incomplete_hex\": \"\\u12\"}",
        "{\"unpaired_high_surrogate\": \"\\ud800\"}",
        "{\"unpaired_low_surrogate\": \"\\udc00\"}",
        "{\"bad_number\": 1.2.3}",
        "{\"bad_number_2\": +123}",
        "{\"bad_number_3\": 0123}",
        "{\"bad_number_4\": 1e}",
        "{\"nan\": NaN}",
        "{\"infinity\": Infinity}",
        "{\"null_byte\": \"\x01\"}"
    };

    for (const auto& badInput : malformedInputs) {
        JsonValue val;
        bool res = JsonValue::parse(badInput, val);
        TEST_ASSERT_MSG(!res, ("Input should have failed: " + badInput).c_str());
        TEST_ASSERT(val.is_null());
    }
}

TEST_CASE(JsonLimits, MalformedFixtureRejection) {
    std::wstring fixturePath = GetExeDir() + L"\\tests\\fixtures\\malformed.json";
    if (!FileExists(fixturePath)) {
        fixturePath = L"tests\\fixtures\\malformed.json";
    }
    if (!FileExists(fixturePath)) {
        fixturePath = L"..\\tests\\fixtures\\malformed.json";
    }
    if (!FileExists(fixturePath)) {
        fixturePath = L"..\\..\\tests\\fixtures\\malformed.json";
    }

    std::string content;
    if (ReadTextFile(fixturePath, content).is_ok()) {
        JsonValue parsed;
        TEST_ASSERT(!JsonValue::parse(content, parsed));
        TEST_ASSERT(parsed.is_null());
    }
}

TEST_CASE(JsonLimits, StringLimitRejection) {
    std::string bigStr = "\"";
    bigStr.append(1000, 'A');
    bigStr += "\"";
    JsonValue okStrVal;
    TEST_ASSERT(JsonValue::parse(bigStr, okStrVal));
    TEST_ASSERT_EQ(okStrVal.as_string().size(), 1000);
}

TEST_CASE(JsonLimits, SettingsAndMediaLibMalformedJsonHandling) {
    ScopedTempDir tempDir(L"Aperture_BadJson_Test_");
    std::wstring dataDir = tempDir.SubPath(L"data");
    CreateDirectoryW(dataDir.c_str(), NULL);

    // Write malformed JSON
    std::wstring settingsPath = dataDir + L"\\settings.json";
    tempDir.WriteFile(L"data\\settings.json", "{ \"themePreset\": \"oled\", unclosed");

    std::string content;
    TEST_ASSERT(ReadTextFile(settingsPath, content).is_ok());
    JsonValue out;
    TEST_ASSERT(!JsonValue::parse(content, out));

    // StorageErrorCode verification
    StorageStatus err = StorageStatus::Error(StorageErrorCode::SerializationFailed, ERROR_INVALID_DATA, settingsPath, "Malformed JSON in settings.json");
    TEST_ASSERT(!err.is_ok());
    TEST_ASSERT_EQ(err.code, StorageErrorCode::SerializationFailed);
}

TEST_CASE(JsonLimits, StrictParseFunctionality) {
    JsonValue out;
    TEST_ASSERT(JsonValue::parse("{\"valid\": [1, 2, 3], \"flag\": true, \"str\": \"ok\"}", out));
    TEST_ASSERT(out.is_object());
    TEST_ASSERT(out.get("valid").is_array());
    TEST_ASSERT_EQ(out.get("valid").as_array().size(), 3);
    TEST_ASSERT_EQ(out.get("flag").as_bool(), true);
    TEST_ASSERT_EQ(out.get("str").as_string(), "ok");

    // Legacy parse overload returns null JsonValue on error
    JsonValue legacyOut = JsonValue::parse("bad { json");
    TEST_ASSERT(legacyOut.is_null());
}
} // namespace Aperture::Test
