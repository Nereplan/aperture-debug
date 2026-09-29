#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/SubtitleDiscovery.h"
#include "../../src/Subtitles.h"
#include <fstream>

namespace Aperture::Test {

TEST_CASE(SubtitleDiscovery, PathNormalization) {
    // Forward slashes to backslashes
    std::wstring p1 = SubtitleDiscovery::NormalizePath(L"C:/media/movies/movie.srt");
    TEST_ASSERT_EQ(p1, std::wstring(L"C:\\media\\movies\\movie.srt"));

    // Relative elements . and ..
    std::wstring p2 = SubtitleDiscovery::NormalizePath(L"C:\\media\\subs\\..\\movies\\.\\movie.srt");
    TEST_ASSERT_EQ(p2, std::wstring(L"C:\\media\\movies\\movie.srt"));

    // Whitespace trimming
    std::wstring p3 = SubtitleDiscovery::NormalizePath(L"   D:\\video\\sample.vtt   \t");
    TEST_ASSERT_EQ(p3, std::wstring(L"D:\\video\\sample.vtt"));

    // Reject control characters and invalid chars
    TEST_ASSERT_EQ(SubtitleDiscovery::NormalizePath(L"C:\\video\\movie*.srt"), std::wstring(L""));
    TEST_ASSERT_EQ(SubtitleDiscovery::NormalizePath(L"C:\\video\\movie?.srt"), std::wstring(L""));
    TEST_ASSERT_EQ(SubtitleDiscovery::NormalizePath(L"C:\\video\\movie|test.srt"), std::wstring(L""));
    TEST_ASSERT_EQ(SubtitleDiscovery::NormalizePath(L"C:\\video\\movie\x01.srt"), std::wstring(L""));
}

TEST_CASE(SubtitleDiscovery, ReservedDeviceNameRejection) {
    TEST_ASSERT(SubtitleDiscovery::IsReservedDeviceName(L"CON"));
    TEST_ASSERT(SubtitleDiscovery::IsReservedDeviceName(L"con.srt"));
    TEST_ASSERT(SubtitleDiscovery::IsReservedDeviceName(L"PRN.vtt"));
    TEST_ASSERT(SubtitleDiscovery::IsReservedDeviceName(L"AUX.ass"));
    TEST_ASSERT(SubtitleDiscovery::IsReservedDeviceName(L"NUL.txt"));
    TEST_ASSERT(SubtitleDiscovery::IsReservedDeviceName(L"COM1.srt"));
    TEST_ASSERT(SubtitleDiscovery::IsReservedDeviceName(L"LPT9.sub"));
    TEST_ASSERT(SubtitleDiscovery::IsReservedDeviceName(L"C:\\media\\NUL.srt"));

    TEST_ASSERT(!SubtitleDiscovery::IsReservedDeviceName(L"C:\\media\\movie.srt"));
    TEST_ASSERT(!SubtitleDiscovery::IsReservedDeviceName(L"console_log.srt"));
    TEST_ASSERT(!SubtitleDiscovery::IsReservedDeviceName(L"connect.vtt"));

    // NormalizePath rejects reserved device names
    TEST_ASSERT_EQ(SubtitleDiscovery::NormalizePath(L"C:\\media\\CON.srt"), std::wstring(L""));
    TEST_ASSERT_EQ(SubtitleDiscovery::NormalizePath(L"D:\\NUL"), std::wstring(L""));
}

TEST_CASE(SubtitleDiscovery, PathContainmentSafety) {
    std::wstring base = L"C:\\media";

    // Valid children
    TEST_ASSERT(SubtitleDiscovery::IsPathContained(base, L"C:\\media\\movie.srt"));
    TEST_ASSERT(SubtitleDiscovery::IsPathContained(base, L"C:\\media\\subs\\movie.en.srt"));
    TEST_ASSERT(SubtitleDiscovery::IsPathContained(base, L"C:/media/subtitles/movie.es.vtt"));

    // Traversal escape attacks
    TEST_ASSERT(!SubtitleDiscovery::IsPathContained(base, L"C:\\media\\subs\\..\\..\\Windows\\system32\\cmd.exe"));
    TEST_ASSERT(!SubtitleDiscovery::IsPathContained(base, L"C:\\media\\..\\secret.srt"));

    // Prefix collision safety: C:\media2 is NOT inside C:\media
    TEST_ASSERT(!SubtitleDiscovery::IsPathContained(base, L"C:\\media2\\movie.srt"));
    TEST_ASSERT(!SubtitleDiscovery::IsPathContained(base, L"C:\\media_files\\subs.srt"));
}

TEST_CASE(SubtitleDiscovery, FilenameMatchingAndScoring) {
    std::wstring media = L"Avatar.2009.mkv";

    // Exact stem match in same dir
    int scoreExact = SubtitleDiscovery::CalculateMatchScore(media, L"Avatar.2009.srt", false);
    TEST_ASSERT_EQ(scoreExact, 100);

    // Language suffix match in same dir
    int scoreSuffix = SubtitleDiscovery::CalculateMatchScore(media, L"Avatar.2009.en.srt", false);
    TEST_ASSERT_EQ(scoreSuffix, 90);

    // Subfolder exact match
    int scoreSubExact = SubtitleDiscovery::CalculateMatchScore(media, L"Avatar.2009.srt", true);
    TEST_ASSERT_EQ(scoreSubExact, 95);

    // Subfolder suffix match
    int scoreSubSuffix = SubtitleDiscovery::CalculateMatchScore(media, L"Avatar.2009.en.srt", true);
    TEST_ASSERT_EQ(scoreSubSuffix, 85);

    // Subfolder generic language match (e.g. subs/english.srt)
    int scoreSubLang = SubtitleDiscovery::CalculateMatchScore(media, L"english.srt", true);
    TEST_ASSERT_EQ(scoreSubLang, 75);

    // Sibling subtitle for different media
    int scoreOther = SubtitleDiscovery::CalculateMatchScore(media, L"Titanic.1997.srt", false);
    TEST_ASSERT_EQ(scoreOther, 20);
}

TEST_CASE(SubtitleDiscovery, LanguageTagParsing) {
    TEST_ASSERT_EQ(SubtitleDiscovery::ParseLanguageFromFileName(L"movie.en.srt"), std::wstring(L"English"));
    TEST_ASSERT_EQ(SubtitleDiscovery::ParseLanguageFromFileName(L"movie.eng.vtt"), std::wstring(L"English"));
    TEST_ASSERT_EQ(SubtitleDiscovery::ParseLanguageFromFileName(L"movie_spanish.ass"), std::wstring(L"Spanish"));
    TEST_ASSERT_EQ(SubtitleDiscovery::ParseLanguageFromFileName(L"movie.spa.srt"), std::wstring(L"Spanish"));
    TEST_ASSERT_EQ(SubtitleDiscovery::ParseLanguageFromFileName(L"movie.ja.srt"), std::wstring(L"Japanese"));
    TEST_ASSERT_EQ(SubtitleDiscovery::ParseLanguageFromFileName(L"movie.jpn.srt"), std::wstring(L"Japanese"));
    TEST_ASSERT_EQ(SubtitleDiscovery::ParseLanguageFromFileName(L"movie.de.srt"), std::wstring(L"German"));
    TEST_ASSERT_EQ(SubtitleDiscovery::ParseLanguageFromFileName(L"movie.fr.srt"), std::wstring(L"French"));
    TEST_ASSERT_EQ(SubtitleDiscovery::ParseLanguageFromFileName(L"movie.zh.srt"), std::wstring(L"Chinese"));
    TEST_ASSERT_EQ(SubtitleDiscovery::ParseLanguageFromFileName(L"movie.ko.srt"), std::wstring(L"Korean"));

    // Modifiers: Forced, SDH, CC
    TEST_ASSERT_EQ(SubtitleDiscovery::ParseLanguageFromFileName(L"movie.en.forced.srt"), std::wstring(L"English (Forced)"));
    TEST_ASSERT_EQ(SubtitleDiscovery::ParseLanguageFromFileName(L"movie.eng.sdh.srt"), std::wstring(L"English (SDH)"));
    TEST_ASSERT_EQ(SubtitleDiscovery::ParseLanguageFromFileName(L"movie.en.cc.srt"), std::wstring(L"English (CC)"));
    TEST_ASSERT_EQ(SubtitleDiscovery::ParseLanguageFromFileName(L"movie.forced.srt"), std::wstring(L"Forced"));
}

TEST_CASE(SubtitleDiscovery, BOMAndEncodingDetection) {
    // 1. UTF-8 BOM: 0xEF, 0xBB, 0xBF
    std::string utf8BomData = "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:04,000\r\nHello UTF-8 BOM\r\n";
    SubtitleEncoding enc1 = SubtitleDiscovery::DetectEncoding(utf8BomData);
    TEST_ASSERT_EQ(enc1, SubtitleEncoding::Utf8Bom);
    std::string dec1 = SubtitleDiscovery::DecodeSubtitleBytes(utf8BomData);
    TEST_ASSERT(dec1.find("Hello UTF-8 BOM") != std::string::npos);
    TEST_ASSERT(dec1.rfind("\xEF\xBB\xBF", 0) != 0); // BOM stripped

    // 2. UTF-16 LE BOM: 0xFF, 0xFE
    std::string utf16LeBomData;
    utf16LeBomData.push_back('\xFF');
    utf16LeBomData.push_back('\xFE');
    std::wstring textW = L"1\r\n00:00:01,000 --> 00:00:04,000\r\nHello UTF-16 LE\r\n";
    for (wchar_t c : textW) {
        utf16LeBomData.push_back(static_cast<char>(c & 0xFF));
        utf16LeBomData.push_back(static_cast<char>((c >> 8) & 0xFF));
    }
    SubtitleEncoding enc2 = SubtitleDiscovery::DetectEncoding(utf16LeBomData);
    TEST_ASSERT_EQ(enc2, SubtitleEncoding::Utf16LE);
    std::string dec2 = SubtitleDiscovery::DecodeSubtitleBytes(utf16LeBomData);
    TEST_ASSERT(dec2.find("Hello UTF-16 LE") != std::string::npos);

    // 3. UTF-16 BE BOM: 0xFE, 0xFF
    std::string utf16BeBomData;
    utf16BeBomData.push_back('\xFE');
    utf16BeBomData.push_back('\xFF');
    for (wchar_t c : textW) {
        utf16BeBomData.push_back(static_cast<char>((c >> 8) & 0xFF));
        utf16BeBomData.push_back(static_cast<char>(c & 0xFF));
    }
    SubtitleEncoding enc3 = SubtitleDiscovery::DetectEncoding(utf16BeBomData);
    TEST_ASSERT_EQ(enc3, SubtitleEncoding::Utf16BE);
    std::string dec3 = SubtitleDiscovery::DecodeSubtitleBytes(utf16BeBomData);
    TEST_ASSERT(dec3.find("Hello UTF-16 LE") != std::string::npos);

    // 4. Raw UTF-8 without BOM (with Japanese multi-byte chars)
    std::string rawUtf8 = "1\r\n00:00:01,000 --> 00:00:04,000\r\nこんにちは世界\r\n";
    SubtitleEncoding enc4 = SubtitleDiscovery::DetectEncoding(rawUtf8);
    TEST_ASSERT_EQ(enc4, SubtitleEncoding::Utf8);
    std::string dec4 = SubtitleDiscovery::DecodeSubtitleBytes(rawUtf8);
    TEST_ASSERT_EQ(dec4, rawUtf8);
}

TEST_CASE(SubtitleDiscovery, MalformedAndOversizedRejection) {
    // 1. Oversized rejection (> 32 MB)
    std::string oversized;
    oversized.resize(SubtitleDiscovery::kMaxSubtitleInputBytes + 1024, 'a');
    std::string decOversized = SubtitleDiscovery::DecodeSubtitleBytes(oversized);
    TEST_ASSERT(decOversized.empty());

    // 2. Malformed SRT parsing robustness
    std::string malformedSrt = "Broken timing line\r\n"
                               "00:00:05,000\r\n"
                               "Some text\r\n\r\n"
                               "2\r\n"
                               "00:00:10,000 --> 00:00:15,000\r\n"
                               "<b><i><u>Valid Formatted Text</u></i></b>\r\n\r\n"
                               "3\r\n"
                               "NaN --> Inf\r\n"
                               "Garbage text\r\n";

    std::vector<SubtitleCue> cues;
    bool srtParsed = SubtitleEngine::ParseSrt(malformedSrt, cues);
    TEST_ASSERT(srtParsed);
    TEST_ASSERT_EQ(cues.size(), 1ull);
    TEST_ASSERT_EQ(cues[0].startTimeSec, 10.0);
    TEST_ASSERT_EQ(cues[0].endTimeSec, 15.0);
    TEST_ASSERT_EQ(cues[0].text, std::wstring(L"Valid Formatted Text"));

    // 3. Deeply nested tag protection
    std::string nestedTags = "1\r\n00:00:01,000 --> 00:00:05,000\r\n";
    for (int i = 0; i < 100; ++i) nestedTags += "<b>";
    nestedTags += "Nested";
    for (int i = 0; i < 100; ++i) nestedTags += "</b>";
    std::wstring clean = SubtitleEngine::CleanSubtitleFormatting(nestedTags);
    TEST_ASSERT(clean.find(L"Nested") != std::wstring::npos);
}

TEST_CASE(SubtitleDiscovery, BoundedSubfolderDiscovery) {
    ScopedTempDir temp(L"Aperture_SubDiscovery_");
    std::wstring tempRoot = temp.GetPath();

    // Create folder structure:
    // tempRoot\movie.mkv
    // tempRoot\movie.srt (exact match, score 100)
    // tempRoot\movie.en.srt (suffix match, score 90)
    // tempRoot\subs\movie.es.srt (subfolder suffix match, score 85)
    // tempRoot\subtitles\movie.ja.ass (subfolder suffix match, score 85)
    // tempRoot\deep\nested\deep.srt (depth 2 - must NOT be discovered)
    temp.WriteFile(L"movie.mkv", "dummy video content");
    std::wstring mediaFile = temp.SubPath(L"movie.mkv");
    temp.WriteFile(L"movie.srt", "1\r\n00:00:01,000 --> 00:00:04,000\r\nDefault Subtitle\r\n");
    temp.WriteFile(L"movie.en.srt", "1\r\n00:00:01,000 --> 00:00:04,000\r\nEnglish Subtitle\r\n");
    temp.WriteFile(L"subs\\movie.es.srt", "1\r\n00:00:01,000 --> 00:00:04,000\r\nSpanish Subtitle\r\n");
    temp.WriteFile(L"subtitles\\movie.ja.ass", "[Script Info]\r\nTitle: Test\r\n[Events]\r\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\r\nDialogue: 0,0:00:01.00,0:00:04.00,Default,,0,0,0,,Japanese Subtitle\r\n");
    temp.WriteFile(L"deep\\nested\\deep.srt", "1\r\n00:00:01,000 --> 00:00:04,000\r\nToo Deep Subtitle\r\n");

    // Perform discovery
    std::vector<DiscoveredSubtitle> discovered = SubtitleDiscovery::DiscoverSubtitles(mediaFile);

    // Verify discovered count (should find 4 subtitles: 2 in root + 1 in subs + 1 in subtitles)
    TEST_ASSERT_EQ(discovered.size(), 4ull);

    // Highest score first: movie.srt (score 100)
    TEST_ASSERT_EQ(discovered[0].matchScore, 100);
    TEST_ASSERT(discovered[0].filePath.find(L"movie.srt") != std::wstring::npos);

    // Second: movie.en.srt (score 90)
    TEST_ASSERT_EQ(discovered[1].matchScore, 90);
    TEST_ASSERT_EQ(discovered[1].language, std::wstring(L"English"));

    // Subfolder files (score 85)
    bool hasSpanish = false;
    bool hasJapanese = false;
    for (const auto& d : discovered) {
        if (d.language == L"Spanish" && d.isSubfolder) hasSpanish = true;
        if (d.language == L"Japanese" && d.isSubfolder) hasJapanese = true;
        // Verify no deep nested file
        TEST_ASSERT(d.filePath.find(L"deep.srt") == std::wstring::npos);
    }
    TEST_ASSERT(hasSpanish);
    TEST_ASSERT(hasJapanese);

    // Test SubtitleEngine loading media
    SubtitleEngine engine;
    engine.LoadSubtitlesForMedia(mediaFile);
    TEST_ASSERT_EQ(engine.GetTrackCount(), 4);
    TEST_ASSERT_EQ(engine.GetPrimaryTrackIndex(), 0);

    // Verify cue playback
    std::wstring text = engine.GetPrimarySubtitleText(2.0);
    TEST_ASSERT_EQ(text, std::wstring(L"Default Subtitle"));

    // Select secondary track (Japanese at index 2, Spanish at index 3)
    engine.SelectSecondaryTrack(2);
    TEST_ASSERT_EQ(engine.GetSecondarySubtitleText(2.0), std::wstring(L"Japanese Subtitle"));
    engine.SelectSecondaryTrack(3);
    TEST_ASSERT_EQ(engine.GetSecondarySubtitleText(2.0), std::wstring(L"Spanish Subtitle"));
}

TEST_CASE(SubtitleDiscovery, RootRelativePathPersistence) {
    std::wstring rootDir = L"C:\\PortableApp";

    // Subpath under rootDir
    std::wstring fullPath = L"C:\\PortableApp\\videos\\subs\\movie.en.srt";
    std::wstring rel = SubtitleDiscovery::ToRootRelativePath(fullPath, rootDir);
    TEST_ASSERT_EQ(rel, std::wstring(L"videos\\subs\\movie.en.srt"));

    // Reconstruct from relative
    std::wstring restored = SubtitleDiscovery::FromRootRelativePath(rel, rootDir);
    TEST_ASSERT_EQ(restored, std::wstring(L"C:\\PortableApp\\videos\\subs\\movie.en.srt"));

    // Traversal attack relative path is rejected
    std::wstring evilRel = L"..\\..\\Windows\\system32\\malicious.srt";
    std::wstring evilRestored = SubtitleDiscovery::FromRootRelativePath(evilRel, rootDir);
    TEST_ASSERT_EQ(evilRestored, std::wstring(L""));
}

} // namespace Aperture::Test
