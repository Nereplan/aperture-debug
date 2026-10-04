#include "../../src/Subtitles.h"
#include "../../src/Common.h"

#include <iostream>
#include <vector>
#include <string>
#include <random>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

using namespace Aperture;

namespace {

// --- Seed Corpora ---

const char* kValidSrt =
    "1\r\n"
    "00:00:01,000 --> 00:00:04,000\r\n"
    "Hello <i>World</i> &amp; welcome!\r\n\r\n"
    "2\r\n"
    "00:00:05,500 --> 00:00:08,200\r\n"
    "<b>Second cue</b> with <font color=\"#ff0000\">colored text</font><br>and a line break.\r\n\r\n"
    "3\r\n"
    "01:23:45,678 --> 01:23:50,123\r\n"
    "Late cue with &#39;quotes&#39; and &quot;entities&quot;.\r\n";

const char* kValidVtt =
    "WEBVTT\r\n\r\n"
    "NOTE Commentary block that should be skipped\r\n\r\n"
    "STYLE\r\n"
    "::cue { color: yellow; }\r\n\r\n"
    "cue-1\r\n"
    "00:01.000 --> 00:04.500 position:10% align:start\r\n"
    "Welcome to <b>Aperture</b> WebVTT!\r\n\r\n"
    "cue-2\r\n"
    "01:02:03.456 --> 01:02:07.890\r\n"
    "Line 1<br/>Line 2&nbsp;&lt;escaped&gt;\r\n";

const char* kValidAss =
    "[Script Info]\r\n"
    "Title: Test Subtitles\r\n"
    "ScriptType: v4.00+\r\n\r\n"
    "[V4+ Styles]\r\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\r\n"
    "Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\r\n\r\n"
    "[Events]\r\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\r\n"
    "Dialogue: 0,0:00:01.20,0:00:03.50,Default,,0,0,0,,{\\pos(100,200)\\b1}First dialogue\\Nwith newline\r\n"
    "Dialogue: 0,0:00:04.00,0:00:07.00,Default,,0,0,0,,{\\i1}Second{\\i0} cue with &amp; entity and \\h hard space\r\n"
    "Dialogue: 1,0:00:06.00,0:00:09.00,Default,,0,0,0,,Overlapping ASS subtitle cue\r\n";

std::string MakeUtf16LeWithBom(const std::string& utf8) {
    std::wstring wide = Utf8ToUtf16(utf8);
    std::string bytes;
    bytes.reserve(2 + wide.size() * 2);
    bytes.push_back(static_cast<char>(0xFF));
    bytes.push_back(static_cast<char>(0xFE));
    for (wchar_t wc : wide) {
        bytes.push_back(static_cast<char>(wc & 0xFF));
        bytes.push_back(static_cast<char>((wc >> 8) & 0xFF));
    }
    return bytes;
}

std::string MakeUtf16BeWithBom(const std::string& utf8) {
    std::wstring wide = Utf8ToUtf16(utf8);
    std::string bytes;
    bytes.reserve(2 + wide.size() * 2);
    bytes.push_back(static_cast<char>(0xFE));
    bytes.push_back(static_cast<char>(0xFF));
    for (wchar_t wc : wide) {
        bytes.push_back(static_cast<char>((wc >> 8) & 0xFF));
        bytes.push_back(static_cast<char>(wc & 0xFF));
    }
    return bytes;
}

std::string MakeUtf8WithBom(const std::string& utf8) {
    std::string bytes;
    bytes.reserve(3 + utf8.size());
    bytes.push_back(static_cast<char>(0xEF));
    bytes.push_back(static_cast<char>(0xBB));
    bytes.push_back(static_cast<char>(0xBF));
    bytes.append(utf8);
    return bytes;
}

// --- Deterministic Mutator ---

enum class MutationCategory {
    Timestamp,
    Formatting,
    LineEnding,
    EncodingByte,
    HugeCue,
    GeneralByte
};

class SubtitleMutator {
public:
    explicit SubtitleMutator(uint64_t seed) : rng_(seed) {}

    uint32_t RandRange(uint32_t minVal, uint32_t maxVal) {
        if (minVal >= maxVal) return minVal;
        std::uniform_int_distribution<uint32_t> dist(minVal, maxVal);
        return dist(rng_);
    }

    double RandDouble(double minVal, double maxVal) {
        std::uniform_real_distribution<double> dist(minVal, maxVal);
        return dist(rng_);
    }

    std::string Mutate(const std::string& input, MutationCategory category) {
        switch (category) {
        case MutationCategory::Timestamp:
            return MutateTimestamp(input);
        case MutationCategory::Formatting:
            return MutateFormatting(input);
        case MutationCategory::LineEnding:
            return MutateLineEnding(input);
        case MutationCategory::EncodingByte:
            return MutateEncodingByte(input);
        case MutationCategory::HugeCue:
            return MutateHugeCue(input);
        case MutationCategory::GeneralByte:
        default:
            return MutateGeneralByte(input);
        }
    }

private:
    std::mt19937_64 rng_;

    std::string MutateTimestamp(const std::string& input) {
        std::string res = input;
        static const char* kBadTimestamps[] = {
            "00:00:00:00:00",
            "::::",
            "00:99:99,999",
            "-01:00:00,000",
            "00:-05:00.000",
            "1e308:00:00",
            "nan:nan:nan",
            "inf:00:00",
            "-inf:00:00",
            "999999999999999999999999:00:00",
            "00:00:00,000 --> ",
            " --> 00:01:00,000",
            "00:05:00,000 --> 00:01:00,000",
            "00:00:00,000 --> 00:00:00,000",
            "0:0.0 --> 0:0.0",
            "12345678901234567890",
            "00:00:00.0000000000000000000001",
            "00:00:00,000 --> 9999999999.999"
        };

        size_t choice = RandRange(0, sizeof(kBadTimestamps) / sizeof(kBadTimestamps[0]) - 1);
        const char* badTs = kBadTimestamps[choice];

        size_t arrowPos = res.find("-->");
        if (arrowPos != std::string::npos) {
            size_t lineStart = res.rfind('\n', arrowPos);
            lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
            size_t lineEnd = res.find('\n', arrowPos);
            if (lineEnd == std::string::npos) lineEnd = res.size();
            res.replace(lineStart, lineEnd - lineStart, badTs);
        } else {
            size_t commaPos = res.find(',');
            if (commaPos != std::string::npos) {
                res.insert(commaPos, badTs);
            } else {
                res += std::string("\n") + badTs;
            }
        }
        return res;
    }

    std::string MutateFormatting(const std::string& input) {
        std::string res = input;
        static const char* kBadFormatting[] = {
            "<b><i><u><font color=\"red\"><span><a href=\"#\"><div><p>",
            "<<<<<<<<<<<<<<<<<<<<<<<<",
            ">>>>>>>>>>>>>>>>>>>>>>>>",
            "<br<br<br<br<br<br>",
            "{\\pos(100,200)\\an8\\b1\\i1\\u1\\s1\\bord5\\shad3\\c&H0000FF&\\3c&H00FF00&}",
            "{\\{\\{\\{\\{\\{\\{\\{\\",
            "}}}}}}}}}}}}}}}}",
            "&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;",
            "&lt;&gt;&quot;&#39;&apos;&nbsp;&unknown;&;;;;",
            "\\N\\N\\N\\N\\n\\n\\n\\n\\h\\h\\h\\h\\\\\\\\\\\\",
            "<font size=\"999999999\" color=\"#GGGGGG\">",
            "{\\tag_without_closing",
            "<tag without closing",
            "\\0\\0\\0\\0"
        };

        size_t choice = RandRange(0, sizeof(kBadFormatting) / sizeof(kBadFormatting[0]) - 1);
        const char* snippet = kBadFormatting[choice];
        size_t pos = RandRange(0, static_cast<uint32_t>(res.size()));
        res.insert(pos, snippet);
        return res;
    }

    std::string MutateLineEnding(const std::string& input) {
        std::string res;
        res.reserve(input.size() * 2);
        static const char* kLineEndings[] = { "\r\n", "\n", "\r", "\r\r\n", "\n\r", "\n\n\n", "\r\n\r\n\r\n" };

        for (char ch : input) {
            if (ch == '\n' || ch == '\r') {
                size_t choice = RandRange(0, sizeof(kLineEndings) / sizeof(kLineEndings[0]) - 1);
                res.append(kLineEndings[choice]);
            } else {
                res.push_back(ch);
            }
        }
        return res;
    }

    std::string MutateEncodingByte(const std::string& input) {
        std::string res = input;
        uint32_t mode = RandRange(0, 5);
        if (mode == 0) {
            // Truncate or corrupt BOM
            if (res.size() >= 2) res[0] = static_cast<char>(0xFF);
            if (res.size() >= 3) res[1] = static_cast<char>(0xAA);
        } else if (mode == 1) {
            // Insert null bytes
            for (int i = 0; i < 5 && !res.empty(); ++i) {
                size_t pos = RandRange(0, static_cast<uint32_t>(res.size() - 1));
                res[pos] = '\0';
            }
        } else if (mode == 2) {
            // Insert invalid UTF-8 multibyte sequences
            static const unsigned char kInvalidUtf8[] = { 0xC0, 0xAF, 0xFE, 0xFF, 0x80, 0xBF, 0xF5, 0xFF };
            for (int i = 0; i < 4; ++i) {
                size_t pos = RandRange(0, static_cast<uint32_t>(res.size()));
                res.insert(pos, 1, static_cast<char>(kInvalidUtf8[RandRange(0, 7)]));
            }
        } else if (mode == 3) {
            // High-ASCII bytes (CP_ACP simulation)
            for (int i = 0; i < 10 && !res.empty(); ++i) {
                size_t pos = RandRange(0, static_cast<uint32_t>(res.size() - 1));
                res[pos] = static_cast<char>(RandRange(0x80, 0xFF));
            }
        } else if (mode == 4) {
            // Wrap in UTF-16 with flipped endianness
            res = MakeUtf16LeWithBom(res);
            if (res.size() >= 2) std::swap(res[0], res[1]);
        } else {
            // Half wide characters (odd byte length)
            res = MakeUtf16LeWithBom(res);
            if (res.size() > 3) res.pop_back();
        }
        return res;
    }

    std::string MutateHugeCue(const std::string& input) {
        std::string res = input;
        uint32_t mode = RandRange(0, 3);
        if (mode == 0) {
            // Create a huge cue line (100 KB)
            std::string hugeLine(100 * 1024, 'A');
            size_t pos = RandRange(0, static_cast<uint32_t>(res.size()));
            res.insert(pos, hugeLine);
        } else if (mode == 1) {
            // Repeat existing text multiple times
            std::string dup = res;
            for (int i = 0; i < 20; ++i) {
                res += "\r\n" + dup;
            }
        } else if (mode == 2) {
            // Thousand dialogue lines
            std::ostringstream oss;
            for (int i = 0; i < 2000; ++i) {
                oss << "Dialogue: 0,0:00:" << (i % 60) << ".00,0:00:" << ((i + 1) % 60) << ".00,Default,,0,0,0,,Line " << i << "\r\n";
            }
            res += oss.str();
        } else {
            // Thousands of SRT cues
            std::ostringstream oss;
            for (int i = 1; i <= 2000; ++i) {
                oss << i << "\r\n00:00:" << (i % 60) << ",000 --> 00:00:" << ((i + 1) % 60) << ",000\r\nCue number " << i << "\r\n\r\n";
            }
            res += oss.str();
        }
        return res;
    }

    std::string MutateGeneralByte(const std::string& input) {
        if (input.empty()) return "1\r\n00:00:01,000 --> 00:00:02,000\r\nTest\r\n";
        std::string res = input;
        uint32_t ops = RandRange(1, 6);
        for (uint32_t op = 0; op < ops; ++op) {
            uint32_t type = RandRange(0, 4);
            if (type == 0 && !res.empty()) {
                // Byte flip
                size_t pos = RandRange(0, static_cast<uint32_t>(res.size() - 1));
                res[pos] = static_cast<char>(RandRange(0, 255));
            } else if (type == 1 && res.size() > 1) {
                // Byte deletion
                size_t pos = RandRange(0, static_cast<uint32_t>(res.size() - 1));
                res.erase(pos, 1);
            } else if (type == 2) {
                // Byte insertion
                size_t pos = RandRange(0, static_cast<uint32_t>(res.size()));
                res.insert(pos, 1, static_cast<char>(RandRange(0, 255)));
            } else if (type == 3 && res.size() > 2) {
                // Byte swap
                size_t pos1 = RandRange(0, static_cast<uint32_t>(res.size() - 1));
                size_t pos2 = RandRange(0, static_cast<uint32_t>(res.size() - 1));
                std::swap(res[pos1], res[pos2]);
            } else {
                // Substring duplicate
                if (res.size() > 8) {
                    size_t pos = RandRange(0, static_cast<uint32_t>(res.size() - 8));
                    size_t len = RandRange(1, 8);
                    std::string chunk = res.substr(pos, len);
                    res.insert(RandRange(0, static_cast<uint32_t>(res.size())), chunk);
                }
            }
        }
        return res;
    }
};

// --- Invariant Checking Functions ---

bool VerifyCueInvariants(const std::vector<SubtitleCue>& cues) {
    if (cues.size() > SubtitleEngine::kMaxSubtitleCueCount) return false;

    double lastStart = -1.0;
    for (const auto& cue : cues) {
        // 1. Timestamps must be non-negative and finite
        if (!std::isfinite(cue.startTimeSec) || !std::isfinite(cue.endTimeSec)) return false;
        if (cue.startTimeSec < 0.0 || cue.endTimeSec < 0.0) return false;
        if (cue.endTimeSec < cue.startTimeSec) return false;

        // 2. Text must not be empty
        if (cue.text.empty()) return false;
        // 3. Text size must be within cue text limit
        if (cue.text.size() > SubtitleEngine::kMaxCueTextBytes) return false;

        // 4. Cues must be sorted by start time
        if (cue.startTimeSec < lastStart) return false;
        lastStart = cue.startTimeSec;
    }
    return true;
}

void TestSeedParsing() {
    std::cout << "[INFO] Testing valid seed parsing..." << std::endl;

    // 1. SRT Seed
    std::vector<SubtitleCue> srtCues;
    bool srtOk = SubtitleEngine::ParseSrt(kValidSrt, srtCues);
    if (!srtOk || srtCues.size() != 3) {
        std::cerr << "[FAIL] Valid SRT seed failed to parse! Cues: " << srtCues.size() << std::endl;
        std::exit(1);
    }
    if (!VerifyCueInvariants(srtCues)) {
        std::cerr << "[FAIL] Valid SRT cues violated invariants!" << std::endl;
        std::exit(1);
    }
    // Verify formatting cleanup
    if (srtCues[0].text.find(L"<i>") != std::wstring::npos || srtCues[0].text.find(L"&amp;") != std::wstring::npos) {
        std::cerr << "[FAIL] Formatting tags or entities not cleaned in SRT cue 1!" << std::endl;
        std::exit(1);
    }

    // 2. VTT Seed
    std::vector<SubtitleCue> vttCues;
    bool vttOk = SubtitleEngine::ParseVtt(kValidVtt, vttCues);
    if (!vttOk || vttCues.size() != 2) {
        std::cerr << "[FAIL] Valid VTT seed failed to parse! Cues: " << vttCues.size() << std::endl;
        std::exit(1);
    }
    if (!VerifyCueInvariants(vttCues)) {
        std::cerr << "[FAIL] Valid VTT cues violated invariants!" << std::endl;
        std::exit(1);
    }

    // 3. ASS Seed
    std::vector<SubtitleCue> assCues;
    bool assOk = SubtitleEngine::ParseAss(kValidAss, assCues);
    if (!assOk || assCues.size() != 3) {
        std::cerr << "[FAIL] Valid ASS seed failed to parse! Cues: " << assCues.size() << std::endl;
        std::exit(1);
    }
    if (!VerifyCueInvariants(assCues)) {
        std::cerr << "[FAIL] Valid ASS cues violated invariants!" << std::endl;
        std::exit(1);
    }

    // 4. Encoded Seeds
    std::string utf16Le = MakeUtf16LeWithBom(kValidSrt);
    std::string decodedLe = SubtitleEngine::DecodeSubtitleBytes(utf16Le);
    std::vector<SubtitleCue> utf16Cues;
    if (!SubtitleEngine::ParseSrt(decodedLe, utf16Cues) || utf16Cues.size() != 3) {
        std::cerr << "[FAIL] UTF-16 LE BOM seed failed to decode and parse!" << std::endl;
        std::exit(1);
    }

    std::string utf16Be = MakeUtf16BeWithBom(kValidSrt);
    std::string decodedBe = SubtitleEngine::DecodeSubtitleBytes(utf16Be);
    std::vector<SubtitleCue> utf16BeCues;
    if (!SubtitleEngine::ParseSrt(decodedBe, utf16BeCues) || utf16BeCues.size() != 3) {
        std::cerr << "[FAIL] UTF-16 BE BOM seed failed to decode and parse!" << std::endl;
        std::exit(1);
    }

    std::string utf8Bom = MakeUtf8WithBom(kValidVtt);
    std::string decodedUtf8 = SubtitleEngine::DecodeSubtitleBytes(utf8Bom);
    std::vector<SubtitleCue> utf8BomCues;
    if (!SubtitleEngine::ParseVtt(decodedUtf8, utf8BomCues) || utf8BomCues.size() != 2) {
        std::cerr << "[FAIL] UTF-8 BOM seed failed to decode and parse!" << std::endl;
        std::exit(1);
    }

    std::cout << "[PASS] All baseline seeds parsed successfully and preserved behavior." << std::endl;
}

void TestExplicitLimits() {
    std::cout << "[INFO] Testing explicit limit rejections..." << std::endl;

    // 1. Oversized Input (> 32 MB)
    {
        std::string oversized(SubtitleEngine::kMaxSubtitleInputBytes + 1024, ' ');
        std::vector<SubtitleCue> cues;
        bool srtRes = SubtitleEngine::ParseSrt(oversized, cues);
        if (srtRes || !cues.empty()) {
            std::cerr << "[FAIL] Oversized SRT input was not rejected!" << std::endl;
            std::exit(1);
        }
        bool assRes = SubtitleEngine::ParseAss(oversized, cues);
        if (assRes || !cues.empty()) {
            std::cerr << "[FAIL] Oversized ASS input was not rejected!" << std::endl;
            std::exit(1);
        }
        std::string decoded = SubtitleEngine::DecodeSubtitleBytes(oversized);
        if (!decoded.empty()) {
            std::cerr << "[FAIL] Oversized input in DecodeSubtitleBytes was not rejected!" << std::endl;
            std::exit(1);
        }
    }

    // 2. Empty Input
    {
        std::vector<SubtitleCue> cues;
        if (SubtitleEngine::ParseSrt("", cues) || !cues.empty()) {
            std::cerr << "[FAIL] Empty SRT input accepted!" << std::endl;
            std::exit(1);
        }
        if (SubtitleEngine::ParseVtt("", cues) || !cues.empty()) {
            std::cerr << "[FAIL] Empty VTT input accepted!" << std::endl;
            std::exit(1);
        }
        if (SubtitleEngine::ParseAss("", cues) || !cues.empty()) {
            std::cerr << "[FAIL] Empty ASS input accepted!" << std::endl;
            std::exit(1);
        }
    }

    // 3. Huge Single Cue Text (> 128 KB)
    {
        std::string hugeText(SubtitleEngine::kMaxCueTextBytes + 10000, 'X');
        std::string srtWithHugeText = "1\r\n00:00:01,000 --> 00:00:05,000\r\n" + hugeText + "\r\n";
        std::vector<SubtitleCue> cues;
        bool ok = SubtitleEngine::ParseSrt(srtWithHugeText, cues);
        if (ok) {
            if (cues[0].text.size() > SubtitleEngine::kMaxCueTextBytes) {
                std::cerr << "[FAIL] Cue text was not bounded to kMaxCueTextBytes! Size: " << cues[0].text.size() << std::endl;
                std::exit(1);
            }
        }
    }

    // 4. Pathological Formatting Work / Nesting
    {
        std::string deepTags;
        for (int i = 0; i < 50000; ++i) deepTags += "<b>";
        deepTags += "hello";
        for (int i = 0; i < 50000; ++i) deepTags += "</b>";

        std::wstring cleaned = SubtitleEngine::CleanSubtitleFormatting(deepTags);
        // Must complete within bounded time and without memory blowup
        if (cleaned.size() > SubtitleEngine::kMaxCueTextBytes) {
            std::cerr << "[FAIL] CleanSubtitleFormatting exceeded max text size!" << std::endl;
            std::exit(1);
        }

        std::string entityBomb;
        for (int i = 0; i < 20000; ++i) entityBomb += "&amp;";
        std::wstring cleanedEntities = SubtitleEngine::CleanSubtitleFormatting(entityBomb);
        if (cleanedEntities.size() > SubtitleEngine::kMaxCueTextBytes) {
            std::cerr << "[FAIL] Entity unescaping exceeded bounds!" << std::endl;
            std::exit(1);
        }
    }

    // 5. Malformed Timestamps
    {
        for (const char* malformed : { "999999999999999999999999999:00:00", "nan:inf:123", "-10:-20:-30" }) {
            // ParseTimestamp uses -1 to distinguish rejection from a valid zero.
            if (SubtitleEngine::ParseTimestamp(malformed) != -1.0) {
                std::cerr << "[FAIL] Malformed timestamp was not rejected: " << malformed << std::endl;
                std::exit(1);
            }
            std::vector<SubtitleCue> cues;
            std::string srt = std::string("1\n") + malformed + " --> 00:00:05,000\nInvalid cue\n";
            if (SubtitleEngine::ParseSrt(srt, cues) || !cues.empty()) {
                std::cerr << "[FAIL] Malformed timestamp created an SRT cue: " << malformed << std::endl;
                std::exit(1);
            }
        }
    }

    std::cout << "[PASS] Explicit limits and boundary conditions verified." << std::endl;
}

void TestEnginePlaybackQueries() {
    std::cout << "[INFO] Testing SubtitleEngine playback queries & overlap handling..." << std::endl;

    SubtitleEngine engine;
    if (!engine.GetCurrentSubtitleText(0.0).empty()) {
        std::cerr << "[FAIL] Empty engine returned non-empty subtitle text!" << std::endl;
        std::exit(1);
    }

    // Load synthetic track
    SubtitleTrack track;
    track.id = "test_track";
    track.name = L"Test Track";

    SubtitleCue c1; c1.startTimeSec = 1.0; c1.endTimeSec = 4.0; c1.text = L"First Cue";
    SubtitleCue c2; c2.startTimeSec = 2.0; c2.endTimeSec = 5.0; c2.text = L"Second Overlapping Cue";
    SubtitleCue c3; c3.startTimeSec = 10.0; c3.endTimeSec = 15.0; c3.text = L"Third Cue";
    track.cues = { c1, c2, c3 };
    engine.tracks.push_back(track);
    engine.SelectTrack(0);

    // Before any cue
    if (!engine.GetCurrentSubtitleText(0.5).empty()) {
        std::cerr << "[FAIL] Subtitle text present before cue start time!" << std::endl;
        std::exit(1);
    }

    // At 1.5s -> First Cue
    std::wstring text1 = engine.GetCurrentSubtitleText(1.5);
    if (text1 != L"First Cue") {
        std::cerr << "[FAIL] Expected 'First Cue', got: " << Utf16ToUtf8(text1) << std::endl;
        std::exit(1);
    }

    // At 3.0s -> First Cue + Second Overlapping Cue
    std::wstring text2 = engine.GetCurrentSubtitleText(3.0);
    if (text2.find(L"First Cue") == std::wstring::npos || text2.find(L"Second Overlapping Cue") == std::wstring::npos) {
        std::cerr << "[FAIL] Overlapping cues not rendered properly! Got: " << Utf16ToUtf8(text2) << std::endl;
        std::exit(1);
    }

    // Test with delayMs
    std::wstring textWithDelay = engine.GetCurrentSubtitleText(3.5, 500); // 3.5 - 0.5 = 3.0
    if (textWithDelay != text2) {
        std::cerr << "[FAIL] Subtitle delayMs not functioning as expected!" << std::endl;
        std::exit(1);
    }

    // Test negative / NaN playback times
    if (!engine.GetCurrentSubtitleText(-5.0).empty()) {
        std::cerr << "[FAIL] Negative playback time returned text!" << std::endl;
        std::exit(1);
    }
    if (!engine.GetCurrentSubtitleText(std::numeric_limits<double>::quiet_NaN()).empty()) {
        std::cerr << "[FAIL] NaN playback time returned text!" << std::endl;
        std::exit(1);
    }

    std::cout << "[PASS] SubtitleEngine playback queries verified." << std::endl;
}

} // namespace

int main(int argc, char* argv[]) {
    uint64_t seed = 1337;
    size_t iterations = 5000;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--seed" || arg == "-s") {
            if (i + 1 < argc) seed = std::stoull(argv[++i]);
        } else if (arg == "--iterations" || arg == "-n") {
            if (i + 1 < argc) iterations = std::stoull(argv[++i]);
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: subtitle_fuzz.exe [--seed <N>] [--iterations <N>] [--verbose] [--help]" << std::endl;
            return 0;
        }
    }

    std::cout << "========================================================" << std::endl;
    std::cout << "  Aperture Subtitle Engine Deterministic Mutation Fuzz  " << std::endl;
    std::cout << "========================================================" << std::endl;
    std::cout << "Seed: " << seed << " | Iterations: " << iterations << std::endl << std::endl;

    // 1. Run deterministic baseline seed checks
    TestSeedParsing();
    TestExplicitLimits();
    TestEnginePlaybackQueries();

    std::cout << "\n[INFO] Running " << iterations << " deterministic mutation fuzz cycles..." << std::endl;

    SubtitleMutator mutator(seed);
    const std::vector<std::string> seedCorpus = {
        kValidSrt,
        kValidVtt,
        kValidAss,
        MakeUtf16LeWithBom(kValidSrt),
        MakeUtf16BeWithBom(kValidSrt),
        MakeUtf8WithBom(kValidVtt)
    };

    const MutationCategory categories[] = {
        MutationCategory::Timestamp,
        MutationCategory::Formatting,
        MutationCategory::LineEnding,
        MutationCategory::EncodingByte,
        MutationCategory::HugeCue,
        MutationCategory::GeneralByte
    };
    const size_t numCategories = sizeof(categories) / sizeof(categories[0]);

    size_t acceptedSrt = 0;
    size_t acceptedVtt = 0;
    size_t acceptedAss = 0;
    size_t rejectedCount = 0;
    size_t totalCuesParsed = 0;

    auto startTime = std::chrono::high_resolution_clock::now();

    for (size_t iter = 0; iter < iterations; ++iter) {
        // Pick base seed and mutation category deterministically
        const std::string& baseSeed = seedCorpus[iter % seedCorpus.size()];
        MutationCategory cat = categories[iter % numCategories];

        std::string mutated = mutator.Mutate(baseSeed, cat);

        // Sub-mutations for deeper exploration
        if (iter % 3 == 0) {
            mutated = mutator.Mutate(mutated, categories[(iter + 1) % numCategories]);
        }
        if (iter % 7 == 0) {
            mutated = mutator.Mutate(mutated, MutationCategory::GeneralByte);
        }

        // Test 1: Byte Decoding
        std::string decoded = SubtitleEngine::DecodeSubtitleBytes(mutated);
        const std::string& contentToParse = decoded.empty() ? mutated : decoded;

        // Test 2: Invariant Check - Deterministic Parse
        // Parsing the exact same string twice must yield the exact same result
        std::vector<SubtitleCue> cues1;
        std::vector<SubtitleCue> cues2;

        bool srtOk1 = SubtitleEngine::ParseSrt(contentToParse, cues1);
        bool srtOk2 = SubtitleEngine::ParseSrt(contentToParse, cues2);
        if (srtOk1 != srtOk2 || cues1.size() != cues2.size()) {
            std::cerr << "[FATAL] Non-deterministic SRT parse at iteration " << iter << "!" << std::endl;
            return 1;
        }

        if (srtOk1) {
            ++acceptedSrt;
            totalCuesParsed += cues1.size();
            if (!VerifyCueInvariants(cues1)) {
                std::cerr << "[FATAL] SRT cue invariant violation at iteration " << iter << "!" << std::endl;
                return 1;
            }
        }

        // Test 3: VTT Parse
        std::vector<SubtitleCue> vttCues;
        bool vttOk = SubtitleEngine::ParseVtt(contentToParse, vttCues);
        if (vttOk) {
            ++acceptedVtt;
            if (!VerifyCueInvariants(vttCues)) {
                std::cerr << "[FATAL] VTT cue invariant violation at iteration " << iter << "!" << std::endl;
                return 1;
            }
        }

        // Test 4: ASS Parse
        std::vector<SubtitleCue> assCues;
        bool assOk = SubtitleEngine::ParseAss(contentToParse, assCues);
        if (assOk) {
            ++acceptedAss;
            totalCuesParsed += assCues.size();
            if (!VerifyCueInvariants(assCues)) {
                std::cerr << "[FATAL] ASS cue invariant violation at iteration " << iter << "!" << std::endl;
                return 1;
            }
        }

        if (!srtOk1 && !vttOk && !assOk) {
            ++rejectedCount;
        }

        // Test 5: SubtitleEngine Load & Query with mutated cues
        if (srtOk1 || assOk) {
            SubtitleEngine engine;
            SubtitleTrack track;
            track.id = "fuzz_track";
            track.cues = srtOk1 ? cues1 : assCues;
            engine.tracks.push_back(std::move(track));
            engine.SelectTrack(0);

            // Query various playback timestamps
            engine.GetCurrentSubtitleText(0.0);
            engine.GetCurrentSubtitleText(5.0);
            engine.GetCurrentSubtitleText(100.0, 500);
            engine.GetCurrentSubtitleText(99999.0, -500);
        }

        if (verbose && (iter + 1) % 1000 == 0) {
            std::cout << "  Progress: " << (iter + 1) << " / " << iterations << " iterations..." << std::endl;
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    std::cout << "\n========================================================" << std::endl;
    std::cout << "                 Fuzzing Run Summary                    " << std::endl;
    std::cout << "========================================================" << std::endl;
    std::cout << "Total Iterations:   " << iterations << std::endl;
    std::cout << "Elapsed Time:       " << elapsedMs << " ms" << std::endl;
    std::cout << "Accepted SRT Runs:  " << acceptedSrt << std::endl;
    std::cout << "Accepted VTT Runs:  " << acceptedVtt << std::endl;
    std::cout << "Accepted ASS Runs:  " << acceptedAss << std::endl;
    std::cout << "Rejected Mutants:   " << rejectedCount << std::endl;
    std::cout << "Total Cues Validated: " << totalCuesParsed << std::endl;
    std::cout << "Invariant Failures: 0" << std::endl;
    std::cout << "Crashes / Hangs:    0" << std::endl;
    std::cout << "\n[PASS] All fuzzing invariant assertions passed." << std::endl;

    return 0;
}
