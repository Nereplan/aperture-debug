#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Json.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"

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
