#pragma once

#include "../support/TestFramework.h"
#include "../../src/IpcProtocol.h"
#include <windows.h>
#include <vector>
#include <string>

namespace Aperture::Test {
static void step_IpcEnvelope_RoundTripBasic() {
    std::vector<std::wstring> originalPaths = {
        L"C:\\Videos\\Movie.mkv",
        L"D:\\Media\\Track01.flac",
        L"E:\\Anime\\[SubGroup] Show - 01 [1080p].mp4"
    };

    std::vector<uint8_t> envelope = Ipc::SerializeEnvelope(originalPaths, Ipc::Command::OpenFiles, 1);
    TEST_ASSERT(!envelope.empty());
    TEST_ASSERT(envelope.size() > sizeof(Ipc::EnvelopeHeader));

    std::vector<std::wstring> decodedPaths;
    Ipc::EnvelopeHeader header = {};
    Ipc::ValidationResult res = Ipc::DeserializeEnvelope(
        envelope.data(), envelope.size(), decodedPaths, &header, true
    );

    TEST_ASSERT_EQ(res, Ipc::ValidationResult::Success);
    TEST_ASSERT_EQ(header.magic, Ipc::IPC_MAGIC_V2);
    TEST_ASSERT_EQ(header.version, Ipc::IPC_CURRENT_VERSION);
    TEST_ASSERT_EQ(header.fileCount, (uint32_t)originalPaths.size());
    TEST_ASSERT_EQ(header.command, (uint32_t)Ipc::Command::OpenFiles);
    TEST_ASSERT_EQ(header.flags, 1u);
    TEST_ASSERT_EQ(decodedPaths.size(), originalPaths.size());

    for (size_t i = 0; i < originalPaths.size(); ++i) {
        TEST_ASSERT_EQ(decodedPaths[i], originalPaths[i]);
    }
}
static void step_IpcEnvelope_RoundTripUnicodeCharacters() {
    std::vector<std::wstring> unicodePaths = {
        L"C:\\Movies\\\u6771\u4EAC\u7269\u8A9E (Tokyo Story) 1953.mkv",       // Japanese
        L"D:\\Music\\\u0421\u043A\u0440\u044F\u0431\u0438\u043D - \u041F\u043E\u044D\u043C\u0430.flac", // Russian / Cyrillic
        L"E:\\Video\\\u0627\u0644\u0645\u0635\u064A\u0631 (Destiny).mp4",     // Arabic
        L"C:\\Test\\Special \u2014 \u2013 \u00E9\u00E0\u00FC\u00F6\u00DF \U0001F3AC.mkv" // Accents & symbols
    };

    std::vector<uint8_t> envelope = Ipc::SerializeEnvelope(unicodePaths, Ipc::Command::OpenFiles, 1);
    TEST_ASSERT(!envelope.empty());

    std::vector<std::wstring> decoded;
    Ipc::EnvelopeHeader header = {};
    Ipc::ValidationResult res = Ipc::DeserializeEnvelope(
        envelope.data(), envelope.size(), decoded, &header, true
    );

    TEST_ASSERT_EQ(res, Ipc::ValidationResult::Success);
    TEST_ASSERT_EQ(decoded.size(), unicodePaths.size());
    for (size_t i = 0; i < unicodePaths.size(); ++i) {
        TEST_ASSERT_EQ(decoded[i], unicodePaths[i]);
    }
}
static void step_IpcEnvelope_RoundTripLongPaths() {
    // Generate paths far exceeding traditional MAX_PATH (260)
    std::wstring longBase = L"C:\\Very\\Deeply\\Nested\\Directory\\Structure\\With\\Many\\Levels\\Of\\Subfolders\\To\\Test\\Win32\\Long\\Path\\Support\\";
    while (longBase.size() < 400) {
        longBase += L"Segment_With_A_Rather_Long_Name_For_Testing\\";
    }

    std::vector<std::wstring> longPaths = {
        longBase + L"Extremely_Long_Filename_Movie_Release_2026_Direct2D_Aperture_Test_File_Number_001.mkv",
        longBase + L"Second_Long_Filename_Track_Audio_Test_File_Number_002.flac"
    };

    TEST_ASSERT(longPaths[0].size() > 450);

    std::vector<uint8_t> envelope = Ipc::SerializeEnvelope(longPaths, Ipc::Command::OpenFiles, 1);
    TEST_ASSERT(!envelope.empty());

    std::vector<std::wstring> decoded;
    Ipc::ValidationResult res = Ipc::DeserializeEnvelope(
        envelope.data(), envelope.size(), decoded, nullptr, true
    );

    TEST_ASSERT_EQ(res, Ipc::ValidationResult::Success);
    TEST_ASSERT_EQ(decoded.size(), longPaths.size());
    TEST_ASSERT_EQ(decoded[0], longPaths[0]);
    TEST_ASSERT_EQ(decoded[1], longPaths[1]);
}
static void step_IpcEnvelope_RejectOversizedPayload() {
    // Attempt to serialize a payload exceeding MAX_IPC_PAYLOAD_SIZE (2MB)
    std::wstring hugePath(5000, L'A');
    std::vector<std::wstring> hugeList;
    for (size_t i = 0; i < 500; ++i) {
        hugeList.push_back(hugePath);
    }

    std::vector<uint8_t> envelope = Ipc::SerializeEnvelope(hugeList);
    // Should be rejected during serialization
    TEST_ASSERT(envelope.empty());

    // Also test deserialization rejection of oversized buffer
    std::vector<uint8_t> fakeOversized(Ipc::MAX_IPC_PAYLOAD_SIZE + 1024, 0);
    std::vector<std::wstring> out;
    Ipc::ValidationResult res = Ipc::DeserializeEnvelope(fakeOversized.data(), fakeOversized.size(), out);
    TEST_ASSERT_EQ(res, Ipc::ValidationResult::PayloadOversized);
}
static void step_IpcEnvelope_RejectExcessiveFileCount() {
    std::vector<std::wstring> excessiveList(Ipc::MAX_IPC_FILE_COUNT + 1, L"C:\\test.mp4");
    std::vector<uint8_t> envelope = Ipc::SerializeEnvelope(excessiveList);
    TEST_ASSERT(envelope.empty());
}
static void step_IpcEnvelope_RejectSinglePathTooLong() {
    std::wstring overlyLongPath(Ipc::MAX_IPC_PATH_CHARS + 1, L'X');
    std::vector<std::wstring> list = { overlyLongPath };
    std::vector<uint8_t> envelope = Ipc::SerializeEnvelope(list);
    TEST_ASSERT(envelope.empty());
}
static void step_IpcEnvelope_RejectInvalidMagic() {
    std::vector<std::wstring> paths = { L"C:\\test.mkv" };
    std::vector<uint8_t> envelope = Ipc::SerializeEnvelope(paths);
    TEST_ASSERT(!envelope.empty());

    // Corrupt magic
    Ipc::EnvelopeHeader* h = reinterpret_cast<Ipc::EnvelopeHeader*>(envelope.data());
    h->magic = 0xDEADBEEF;

    std::vector<std::wstring> decoded;
    Ipc::ValidationResult res = Ipc::DeserializeEnvelope(envelope.data(), envelope.size(), decoded);
    TEST_ASSERT_EQ(res, Ipc::ValidationResult::InvalidMagic);
}
static void step_IpcEnvelope_RejectUnsupportedVersion() {
    std::vector<std::wstring> paths = { L"C:\\test.mkv" };
    std::vector<uint8_t> envelope = Ipc::SerializeEnvelope(paths);
    TEST_ASSERT(!envelope.empty());

    Ipc::EnvelopeHeader* h = reinterpret_cast<Ipc::EnvelopeHeader*>(envelope.data());
    h->version = 99;

    std::vector<std::wstring> decoded;
    Ipc::ValidationResult res = Ipc::DeserializeEnvelope(envelope.data(), envelope.size(), decoded);
    TEST_ASSERT_EQ(res, Ipc::ValidationResult::UnsupportedVersion);
}
static void step_IpcEnvelope_RejectCorruptedChecksum() {
    std::vector<std::wstring> paths = { L"C:\\test.mkv" };
    std::vector<uint8_t> envelope = Ipc::SerializeEnvelope(paths);
    TEST_ASSERT(!envelope.empty());

    // Flip a bit in the payload
    envelope.back() ^= 0xFF;

    std::vector<std::wstring> decoded;
    Ipc::ValidationResult res = Ipc::DeserializeEnvelope(envelope.data(), envelope.size(), decoded);
    TEST_ASSERT_EQ(res, Ipc::ValidationResult::ChecksumMismatch);
}
static void step_IpcEnvelope_RejectTruncatedPayload() {
    std::vector<std::wstring> paths = { L"C:\\test.mkv", L"D:\\other.flac" };
    std::vector<uint8_t> envelope = Ipc::SerializeEnvelope(paths);
    TEST_ASSERT(!envelope.empty());

    // Truncate message by 5 bytes
    std::vector<std::wstring> decoded;
    Ipc::ValidationResult res = Ipc::DeserializeEnvelope(envelope.data(), envelope.size() - 5, decoded);
    TEST_ASSERT_EQ(res, Ipc::ValidationResult::TotalSizeMismatch);
}
static void step_IpcEnvelope_RejectEmbeddedNulls() {
    std::wstring pathWithNull = L"C:\\test\0inject.mkv";
    pathWithNull.resize(17); // Keep embedded null

    // Manually forge payload with embedded null
    Ipc::EnvelopeHeader header = {};
    header.magic = Ipc::IPC_MAGIC_V2;
    header.version = Ipc::IPC_CURRENT_VERSION;
    header.headerSize = sizeof(Ipc::EnvelopeHeader);
    header.senderPid = GetCurrentProcessId();
    header.command = 1;
    header.fileCount = 1;
    header.flags = 1;

    uint32_t charCount = (uint32_t)pathWithNull.size();
    std::vector<uint8_t> payload;
    const uint8_t* pCount = reinterpret_cast<const uint8_t*>(&charCount);
    payload.insert(payload.end(), pCount, pCount + sizeof(uint32_t));
    const uint8_t* pStr = reinterpret_cast<const uint8_t*>(pathWithNull.data());
    payload.insert(payload.end(), pStr, pStr + (charCount * sizeof(wchar_t)));

    header.totalSize = (uint32_t)(sizeof(Ipc::EnvelopeHeader) + payload.size());
    header.checksum = Ipc::ComputeChecksum(payload.data(), payload.size());

    std::vector<uint8_t> forged(sizeof(Ipc::EnvelopeHeader) + payload.size());
    std::memcpy(forged.data(), &header, sizeof(Ipc::EnvelopeHeader));
    std::memcpy(forged.data() + sizeof(Ipc::EnvelopeHeader), payload.data(), payload.size());

    std::vector<std::wstring> decoded;
    Ipc::ValidationResult res = Ipc::DeserializeEnvelope(forged.data(), forged.size(), decoded, nullptr, true);
    TEST_ASSERT_EQ(res, Ipc::ValidationResult::MalformedPayload);
}
static void step_IpcEnvelope_AppPayloadConversion() {
    std::vector<std::wstring> paths = {
        L"C:\\Path\\To\\File1.mkv",
        L"D:\\Another\\File2.mp4"
    };

    std::wstring appPayload = Ipc::PathsToAppPayload(paths);
    TEST_ASSERT_EQ(appPayload, L"C:\\Path\\To\\File1.mkv\nD:\\Another\\File2.mp4\n");
}
static void step_IpcEnvelope_SameUserAuthenticationCurrentProcess() {
    DWORD currentPid = GetCurrentProcessId();
    TEST_ASSERT(Ipc::IsSameUserAndSession(currentPid));
    // Invalid / zero PID must fail authentication
    TEST_ASSERT(!Ipc::IsSameUserAndSession(0));
}
static void step_CommandLineParser_ParseValidFlagsAll() {
    std::vector<std::wstring> args = {
        L"--start=01:23:45",
        L"--fullscreen",
        L"--paused",
        L"--loop",
        L"--volume=85.5",
        L"--subtitle=C:\\Subs\\movie.srt",
        L"--enqueue",
        L"C:\\Videos\\Movie.mkv"
    };

    Ipc::CommandLineParseOutput out = Ipc::ParseCommandLineArgs(args);
    TEST_ASSERT(out.IsSuccess());
    TEST_ASSERT_EQ(out.result, Ipc::CommandLineParseResult::Success);
    TEST_ASSERT(out.options.startPositionSec.has_value());
    TEST_ASSERT_EQ(*out.options.startPositionSec, 5025.0);
    TEST_ASSERT(out.options.fullscreen);
    TEST_ASSERT(out.options.paused);
    TEST_ASSERT(out.options.loop);
    TEST_ASSERT(out.options.volume.has_value());
    TEST_ASSERT_EQ(*out.options.volume, 85.5);
    TEST_ASSERT(!out.options.subtitlePath.empty());
    TEST_ASSERT(out.options.enqueue);
    TEST_ASSERT_EQ(out.options.mediaTargets.size(), 1u);
}
static void step_CommandLineParser_ParseTimeFormats() {
    struct TestCase {
        std::wstring input;
        double expectedSeconds;
    };

    std::vector<TestCase> cases = {
        { L"0", 0.0 },
        { L"45", 45.0 },
        { L"123.45", 123.45 },
        { L"0.5", 0.5 },
        { L"23:45", 1425.0 },
        { L"01:23:45", 5025.0 },
        { L"00:00:10.5", 10.5 },
        { L"1:05", 65.0 },
        { L"0:30.25", 30.25 },
        { L"2:00:00", 7200.0 }
    };

    for (const auto& tc : cases) {
        double parsed = 0.0;
        std::wstring err;
        bool ok = Ipc::ParseTimeValue(tc.input, parsed, err);
        TEST_ASSERT(ok);
        TEST_ASSERT(std::abs(parsed - tc.expectedSeconds) < 0.001);
    }
}
static void step_CommandLineParser_RejectMalformedTime() {
    std::vector<std::wstring> badInputs = {
        L"-5",
        L"+10",
        L"abc",
        L"1:60:00",  // minutes >= 60
        L"01:23:60", // seconds >= 60
        L"1:2:3:4",  // too many colons
        L":30",
        L"30:",
        L"1::30",
        L"",
        L"12.34.56",
        L"1:2:3.4.5"
    };

    for (const auto& bad : badInputs) {
        double parsed = 0.0;
        std::wstring err;
        bool ok = Ipc::ParseTimeValue(bad, parsed, err);
        TEST_ASSERT(!ok);
        TEST_ASSERT(!err.empty());
    }
}
static void step_CommandLineParser_ParseVolume() {
    std::vector<std::pair<std::wstring, double>> cases = {
        { L"0", 0.0 },
        { L"50", 50.0 },
        { L"100", 100.0 },
        { L"75.25", 75.25 },
        { L"0.0", 0.0 },
        { L"100.0", 100.0 }
    };

    for (const auto& tc : cases) {
        double vol = 0.0;
        std::wstring err;
        bool ok = Ipc::ParseVolumeValue(tc.first, vol, err);
        TEST_ASSERT(ok);
        TEST_ASSERT(std::abs(vol - tc.second) < 0.001);
    }
}
static void step_CommandLineParser_RejectMalformedVolume() {
    std::vector<std::wstring> badVolumes = {
        L"-1",
        L"101",
        L"100.1",
        L"+50",
        L"abc",
        L"50%",
        L"",
        L"50.50.50"
    };

    for (const auto& bad : badVolumes) {
        double vol = 0.0;
        std::wstring err;
        bool ok = Ipc::ParseVolumeValue(bad, vol, err);
        TEST_ASSERT(!ok);
        TEST_ASSERT(!err.empty());
    }
}
static void step_CommandLineParser_ParseBooleanFlags() {
    std::vector<std::wstring> args = {
        L"--fullscreen=true",
        L"--paused=1",
        L"--loop=yes",
        L"--enqueue=on"
    };
    Ipc::CommandLineParseOutput out = Ipc::ParseCommandLineArgs(args);
    TEST_ASSERT(out.IsSuccess());
    TEST_ASSERT(out.options.fullscreen);
    TEST_ASSERT(out.options.paused);
    TEST_ASSERT(out.options.loop);
    TEST_ASSERT(out.options.enqueue);

    std::vector<std::wstring> argsOff = {
        L"--fullscreen=false",
        L"--paused=0",
        L"--loop=no",
        L"--enqueue=off"
    };
    Ipc::CommandLineParseOutput outOff = Ipc::ParseCommandLineArgs(argsOff);
    TEST_ASSERT(outOff.IsSuccess());
    TEST_ASSERT(!outOff.options.fullscreen);
    TEST_ASSERT(!outOff.options.paused);
    TEST_ASSERT(!outOff.options.loop);
    TEST_ASSERT(!outOff.options.enqueue);
}
static void step_CommandLineParser_ValidatedUrlsAndPaths() {
    std::vector<std::wstring> args = {
        L"http://example.com/live/stream.m3u8",
        L"https://video.example.org/feed.mp4",
        L"rtsp://192.168.1.100:554/ch0",
        L"rtmp://live.stream.net/app/key",
        L"C:\\Videos\\Clip.mp4"
    };

    Ipc::CommandLineParseOutput out = Ipc::ParseCommandLineArgs(args);
    TEST_ASSERT(out.IsSuccess());
    TEST_ASSERT_EQ(out.options.mediaTargets.size(), 5u);
    TEST_ASSERT_EQ(out.options.mediaTargets[0], L"http://example.com/live/stream.m3u8");
    TEST_ASSERT_EQ(out.options.mediaTargets[1], L"https://video.example.org/feed.mp4");
    TEST_ASSERT_EQ(out.options.mediaTargets[2], L"rtsp://192.168.1.100:554/ch0");
    TEST_ASSERT_EQ(out.options.mediaTargets[3], L"rtmp://live.stream.net/app/key");
}
static void step_CommandLineParser_OrderIndependence() {
    std::vector<std::wstring> args = {
        L"C:\\Videos\\Movie1.mkv",
        L"--loop",
        L"--volume=70",
        L"https://example.com/live.m3u8",
        L"--fullscreen",
        L"--start=45.5",
        L"D:\\Videos\\Movie2.mkv",
        L"--enqueue"
    };

    Ipc::CommandLineParseOutput out = Ipc::ParseCommandLineArgs(args);
    TEST_ASSERT(out.IsSuccess());
    TEST_ASSERT_EQ(out.options.mediaTargets.size(), 3u);
    TEST_ASSERT(out.options.loop);
    TEST_ASSERT(out.options.fullscreen);
    TEST_ASSERT(out.options.enqueue);
    TEST_ASSERT(out.options.volume.has_value());
    TEST_ASSERT_EQ(*out.options.volume, 70.0);
    TEST_ASSERT(out.options.startPositionSec.has_value());
    TEST_ASSERT_EQ(*out.options.startPositionSec, 45.5);
}
static void step_CommandLineParser_RejectUnknownFlags() {
    std::vector<std::wstring> args = {
        L"--nonexistent-flag=123",
        L"C:\\Videos\\Movie.mkv"
    };
    Ipc::CommandLineParseOutput out = Ipc::ParseCommandLineArgs(args);
    TEST_ASSERT(!out.IsSuccess());
    TEST_ASSERT_EQ(out.result, Ipc::CommandLineParseResult::UnknownFlag);
    TEST_ASSERT(!out.errorMessage.empty());
    // Ensure no partial state was produced
    TEST_ASSERT(out.options.mediaTargets.empty());
}
static void step_CommandLineParser_BoundsChecks() {
    // Too many arguments
    std::vector<std::wstring> hugeArgs(Ipc::MAX_IPC_FILE_COUNT + 1, L"C:\\test.mp4");
    Ipc::CommandLineParseOutput outHuge = Ipc::ParseCommandLineArgs(hugeArgs);
    TEST_ASSERT(!outHuge.IsSuccess());
    TEST_ASSERT_EQ(outHuge.result, Ipc::CommandLineParseResult::TooManyArguments);

    // Argument too long
    std::wstring longArg(Ipc::MAX_IPC_PATH_CHARS + 1, L'A');
    std::vector<std::wstring> longArgs = { longArg };
    Ipc::CommandLineParseOutput outLong = Ipc::ParseCommandLineArgs(longArgs);
    TEST_ASSERT(!outLong.IsSuccess());
    TEST_ASSERT_EQ(outLong.result, Ipc::CommandLineParseResult::ArgumentTooLong);
}
static void step_IpcEnvelope_RoundTripWithOptions() {
    PlaybackCommandLineOptions options;
    options.startPositionSec = 3661.5;
    options.fullscreen = true;
    options.paused = true;
    options.loop = true;
    options.volume = 80.0;
    options.subtitlePath = L"C:\\Subs\\FeatureFilm.eng.srt";
    options.enqueue = true;
    options.mediaTargets = {
        L"C:\\Videos\\FeatureFilm.mkv",
        L"https://streams.example.com/extra.mp4"
    };

    std::vector<uint8_t> envelope = Ipc::SerializeEnvelope(options, Ipc::Command::OpenFiles, 1);
    TEST_ASSERT(!envelope.empty());
    TEST_ASSERT(envelope.size() > sizeof(Ipc::EnvelopeHeader));

    PlaybackCommandLineOptions decoded;
    Ipc::EnvelopeHeader header = {};
    Ipc::ValidationResult res = Ipc::DeserializeEnvelope(
        envelope.data(), envelope.size(), decoded, &header, true
    );

    TEST_ASSERT_EQ(res, Ipc::ValidationResult::Success);
    TEST_ASSERT_EQ(header.magic, Ipc::IPC_MAGIC_V2);
    TEST_ASSERT_EQ(header.version, 2u);
    TEST_ASSERT_EQ(header.fileCount, 2u);

    TEST_ASSERT(decoded.startPositionSec.has_value());
    TEST_ASSERT_EQ(*decoded.startPositionSec, 3661.5);
    TEST_ASSERT(decoded.fullscreen);
    TEST_ASSERT(decoded.paused);
    TEST_ASSERT(decoded.loop);
    TEST_ASSERT(decoded.volume.has_value());
    TEST_ASSERT_EQ(*decoded.volume, 80.0);
    TEST_ASSERT_EQ(decoded.subtitlePath, L"C:\\Subs\\FeatureFilm.eng.srt");
    TEST_ASSERT(decoded.enqueue);
    TEST_ASSERT_EQ(decoded.mediaTargets.size(), 2u);
    TEST_ASSERT_EQ(decoded.mediaTargets[0], L"C:\\Videos\\FeatureFilm.mkv");
    TEST_ASSERT_EQ(decoded.mediaTargets[1], L"https://streams.example.com/extra.mp4");
}
static void step_IpcEnvelope_Version1BackwardCompatibility() {
    // Manually forge a Version 1 envelope (paths only, version = 1)
    std::vector<std::wstring> paths = {
        L"C:\\Legacy\\Video1.mp4",
        L"D:\\Legacy\\Video2.mkv"
    };

    std::vector<uint8_t> payload;
    for (const auto& p : paths) {
        uint32_t charCount = (uint32_t)p.size();
        const uint8_t* pCount = reinterpret_cast<const uint8_t*>(&charCount);
        payload.insert(payload.end(), pCount, pCount + sizeof(uint32_t));
        const uint8_t* pData = reinterpret_cast<const uint8_t*>(p.data());
        payload.insert(payload.end(), pData, pData + (charCount * sizeof(wchar_t)));
    }

    Ipc::EnvelopeHeader header = {};
    header.magic = Ipc::IPC_MAGIC_V2;
    header.version = 1;
    header.headerSize = sizeof(Ipc::EnvelopeHeader);
    header.totalSize = (uint32_t)(sizeof(Ipc::EnvelopeHeader) + payload.size());
    header.senderPid = GetCurrentProcessId();
    header.command = 1;
    header.fileCount = (uint32_t)paths.size();
    header.flags = 1;
    header.checksum = Ipc::ComputeChecksum(payload.data(), payload.size());

    std::vector<uint8_t> v1Envelope(header.totalSize);
    std::memcpy(v1Envelope.data(), &header, sizeof(Ipc::EnvelopeHeader));
    std::memcpy(v1Envelope.data() + sizeof(Ipc::EnvelopeHeader), payload.data(), payload.size());

    PlaybackCommandLineOptions decoded;
    Ipc::ValidationResult res = Ipc::DeserializeEnvelope(
        v1Envelope.data(), v1Envelope.size(), decoded, nullptr, true
    );

    TEST_ASSERT_EQ(res, Ipc::ValidationResult::Success);
    TEST_ASSERT(!decoded.startPositionSec.has_value());
    TEST_ASSERT(!decoded.fullscreen);
    TEST_ASSERT(!decoded.paused);
    TEST_ASSERT(!decoded.loop);
    TEST_ASSERT(!decoded.volume.has_value());
    TEST_ASSERT(decoded.subtitlePath.empty());
    TEST_ASSERT(!decoded.enqueue);
    TEST_ASSERT_EQ(decoded.mediaTargets.size(), 2u);
    TEST_ASSERT_EQ(decoded.mediaTargets[0], paths[0]);
    TEST_ASSERT_EQ(decoded.mediaTargets[1], paths[1]);
}
static void step_AutomationIpc_BasicOperationsRoundTrip() {
    Ipc::AutomationOp ops[] = {
        Ipc::AutomationOp::Play,
        Ipc::AutomationOp::Pause,
        Ipc::AutomationOp::Stop,
        Ipc::AutomationOp::Next,
        Ipc::AutomationOp::Previous,
        Ipc::AutomationOp::QueryStatus
    };

    for (auto op : ops) {
        Ipc::AutomationCommand cmd;
        cmd.op = op;

        std::vector<uint8_t> envelope = Ipc::SerializeAutomationEnvelope(cmd);
        TEST_ASSERT(!envelope.empty());
        TEST_ASSERT(envelope.size() >= sizeof(Ipc::EnvelopeHeader));

        Ipc::AutomationCommand decoded;
        Ipc::EnvelopeHeader header = {};
        Ipc::ValidationResult res = Ipc::DeserializeAutomationEnvelope(
            envelope.data(), envelope.size(), decoded, &header, true
        );

        TEST_ASSERT_EQ(res, Ipc::ValidationResult::Success);
        TEST_ASSERT_EQ(header.magic, Ipc::IPC_MAGIC_V2);
        TEST_ASSERT_EQ(header.version, Ipc::IPC_CURRENT_VERSION);
        TEST_ASSERT_EQ(header.command, (uint32_t)Ipc::Command::Automation);
        TEST_ASSERT_EQ(decoded.op, op);
    }
}
static void step_AutomationIpc_SeekOperations() {
    // 1. Absolute seek
    {
        Ipc::AutomationCommand cmd;
        cmd.op = Ipc::AutomationOp::Seek;
        cmd.seekPositionSec = 125.75;
        cmd.seekRelative = false;

        std::vector<uint8_t> envelope = Ipc::SerializeAutomationEnvelope(cmd);
        TEST_ASSERT(!envelope.empty());

        Ipc::AutomationCommand decoded;
        Ipc::ValidationResult res = Ipc::DeserializeAutomationEnvelope(
            envelope.data(), envelope.size(), decoded, nullptr, true
        );
        TEST_ASSERT_EQ(res, Ipc::ValidationResult::Success);
        TEST_ASSERT_EQ(decoded.op, Ipc::AutomationOp::Seek);
        TEST_ASSERT_EQ(decoded.seekPositionSec, 125.75);
        TEST_ASSERT(!decoded.seekRelative);
    }

    // 2. Relative seek (forward and backward)
    {
        Ipc::AutomationCommand cmd;
        cmd.op = Ipc::AutomationOp::Seek;
        cmd.seekPositionSec = -15.5;
        cmd.seekRelative = true;

        std::vector<uint8_t> envelope = Ipc::SerializeAutomationEnvelope(cmd);
        TEST_ASSERT(!envelope.empty());

        Ipc::AutomationCommand decoded;
        Ipc::ValidationResult res = Ipc::DeserializeAutomationEnvelope(
            envelope.data(), envelope.size(), decoded, nullptr, true
        );
        TEST_ASSERT_EQ(res, Ipc::ValidationResult::Success);
        TEST_ASSERT_EQ(decoded.op, Ipc::AutomationOp::Seek);
        TEST_ASSERT_EQ(decoded.seekPositionSec, -15.5);
        TEST_ASSERT(decoded.seekRelative);
    }
}
static void step_AutomationIpc_VolumeOperations() {
    double testVolumes[] = { 0.0, 25.5, 75.0, 100.0 };
    for (double vol : testVolumes) {
        Ipc::AutomationCommand cmd;
        cmd.op = Ipc::AutomationOp::Volume;
        cmd.volume = vol;

        std::vector<uint8_t> envelope = Ipc::SerializeAutomationEnvelope(cmd);
        TEST_ASSERT(!envelope.empty());

        Ipc::AutomationCommand decoded;
        Ipc::ValidationResult res = Ipc::DeserializeAutomationEnvelope(
            envelope.data(), envelope.size(), decoded, nullptr, true
        );
        TEST_ASSERT_EQ(res, Ipc::ValidationResult::Success);
        TEST_ASSERT_EQ(decoded.op, Ipc::AutomationOp::Volume);
        TEST_ASSERT_EQ(decoded.volume, vol);
    }
}
static void step_AutomationIpc_OpenAndEnqueueOperations() {
    // Open with options
    {
        Ipc::AutomationCommand cmd;
        cmd.op = Ipc::AutomationOp::Open;
        cmd.mediaTargets = { L"C:\\Media\\Movie.mkv", L"https://stream.example.com/video.mp4" };
        cmd.startPositionSec = 45.0;
        cmd.paused = true;
        cmd.fullscreen = true;

        std::vector<uint8_t> envelope = Ipc::SerializeAutomationEnvelope(cmd);
        TEST_ASSERT(!envelope.empty());

        Ipc::AutomationCommand decoded;
        Ipc::ValidationResult res = Ipc::DeserializeAutomationEnvelope(
            envelope.data(), envelope.size(), decoded, nullptr, true
        );
        TEST_ASSERT_EQ(res, Ipc::ValidationResult::Success);
        TEST_ASSERT_EQ(decoded.op, Ipc::AutomationOp::Open);
        TEST_ASSERT_EQ(decoded.startPositionSec, 45.0);
        TEST_ASSERT(decoded.paused);
        TEST_ASSERT(decoded.fullscreen);
        TEST_ASSERT_EQ(decoded.mediaTargets.size(), 2u);
        TEST_ASSERT_EQ(decoded.mediaTargets[1], L"https://stream.example.com/video.mp4");
    }

    // Enqueue
    {
        Ipc::AutomationCommand cmd;
        cmd.op = Ipc::AutomationOp::Enqueue;
        cmd.mediaTargets = { L"D:\\Audio\\Track01.flac", L"D:\\Audio\\Track02.flac" };

        std::vector<uint8_t> envelope = Ipc::SerializeAutomationEnvelope(cmd);
        TEST_ASSERT(!envelope.empty());

        Ipc::AutomationCommand decoded;
        Ipc::ValidationResult res = Ipc::DeserializeAutomationEnvelope(
            envelope.data(), envelope.size(), decoded, nullptr, true
        );
        TEST_ASSERT_EQ(res, Ipc::ValidationResult::Success);
        TEST_ASSERT_EQ(decoded.op, Ipc::AutomationOp::Enqueue);
        TEST_ASSERT_EQ(decoded.mediaTargets.size(), 2u);
    }
}
static void step_AutomationIpc_RejectDisallowedOperations() {
    // 1. None op
    {
        Ipc::AutomationCommand cmd;
        cmd.op = Ipc::AutomationOp::None;
        std::vector<uint8_t> envelope = Ipc::SerializeAutomationEnvelope(cmd);
        TEST_ASSERT(envelope.empty());
    }

    // 2. Out of range op
    {
        Ipc::AutomationCommand cmd;
        cmd.op = static_cast<Ipc::AutomationOp>(999);
        std::vector<uint8_t> envelope = Ipc::SerializeAutomationEnvelope(cmd);
        TEST_ASSERT(envelope.empty());
    }
}
static void step_AutomationIpc_RejectInvalidArguments() {
    // 1. Non-finite seek position
    {
        Ipc::AutomationCommand cmd;
        cmd.op = Ipc::AutomationOp::Seek;
        cmd.seekPositionSec = std::numeric_limits<double>::quiet_NaN();
        TEST_ASSERT(Ipc::SerializeAutomationEnvelope(cmd).empty());

        cmd.seekPositionSec = std::numeric_limits<double>::infinity();
        TEST_ASSERT(Ipc::SerializeAutomationEnvelope(cmd).empty());
    }

    // 2. Negative absolute seek
    {
        Ipc::AutomationCommand cmd;
        cmd.op = Ipc::AutomationOp::Seek;
        cmd.seekPositionSec = -5.0;
        cmd.seekRelative = false;
        TEST_ASSERT(Ipc::SerializeAutomationEnvelope(cmd).empty());
    }

    // 3. Out-of-range volume
    {
        Ipc::AutomationCommand cmd;
        cmd.op = Ipc::AutomationOp::Volume;
        cmd.volume = -1.0;
        TEST_ASSERT(Ipc::SerializeAutomationEnvelope(cmd).empty());

        cmd.volume = 101.0;
        TEST_ASSERT(Ipc::SerializeAutomationEnvelope(cmd).empty());

        cmd.volume = std::numeric_limits<double>::quiet_NaN();
        TEST_ASSERT(Ipc::SerializeAutomationEnvelope(cmd).empty());
    }

    // 4. Empty targets for Open and Enqueue
    {
        Ipc::AutomationCommand cmd;
        cmd.op = Ipc::AutomationOp::Open;
        cmd.mediaTargets.clear();
        TEST_ASSERT(Ipc::SerializeAutomationEnvelope(cmd).empty());

        cmd.op = Ipc::AutomationOp::Enqueue;
        TEST_ASSERT(Ipc::SerializeAutomationEnvelope(cmd).empty());
    }
}
static void step_AutomationIpc_RejectSecurityViolations() {
    // 1. Embedded null in media target
    {
        std::wstring nullPath = L"C:\\Media\\Movie\0inject.mp4";
        nullPath.resize(23);

        Ipc::AutomationCommand cmd;
        cmd.op = Ipc::AutomationOp::Open;
        cmd.mediaTargets = { nullPath };
        TEST_ASSERT(Ipc::SerializeAutomationEnvelope(cmd).empty());
    }

    // 2. Path too long
    {
        std::wstring longPath(Ipc::MAX_IPC_PATH_CHARS + 1, L'A');
        Ipc::AutomationCommand cmd;
        cmd.op = Ipc::AutomationOp::Open;
        cmd.mediaTargets = { longPath };
        TEST_ASSERT(Ipc::SerializeAutomationEnvelope(cmd).empty());
    }

    // 3. Corrupted checksum
    {
        Ipc::AutomationCommand cmd;
        cmd.op = Ipc::AutomationOp::Play;
        std::vector<uint8_t> envelope = Ipc::SerializeAutomationEnvelope(cmd);
        TEST_ASSERT(!envelope.empty());

        envelope.back() ^= 0xFF;

        Ipc::AutomationCommand decoded;
        Ipc::ValidationResult res = Ipc::DeserializeAutomationEnvelope(
            envelope.data(), envelope.size(), decoded, nullptr, true
        );
        TEST_ASSERT_EQ(res, Ipc::ValidationResult::ChecksumMismatch);
    }

    // 4. Invalid magic
    {
        Ipc::AutomationCommand cmd;
        cmd.op = Ipc::AutomationOp::Pause;
        std::vector<uint8_t> envelope = Ipc::SerializeAutomationEnvelope(cmd);
        TEST_ASSERT(!envelope.empty());

        Ipc::EnvelopeHeader* h = reinterpret_cast<Ipc::EnvelopeHeader*>(envelope.data());
        h->magic = 0x12345678;

        Ipc::AutomationCommand decoded;
        Ipc::ValidationResult res = Ipc::DeserializeAutomationEnvelope(
            envelope.data(), envelope.size(), decoded, nullptr, true
        );
        TEST_ASSERT_EQ(res, Ipc::ValidationResult::InvalidMagic);
    }

    // 5. Truncated envelope
    {
        Ipc::AutomationCommand cmd;
        cmd.op = Ipc::AutomationOp::Stop;
        std::vector<uint8_t> envelope = Ipc::SerializeAutomationEnvelope(cmd);
        TEST_ASSERT(!envelope.empty());

        Ipc::AutomationCommand decoded;
        Ipc::ValidationResult res = Ipc::DeserializeAutomationEnvelope(
            envelope.data(), envelope.size() - 4, decoded, nullptr, true
        );
        TEST_ASSERT_EQ(res, Ipc::ValidationResult::TotalSizeMismatch);
    }
}
static void step_AutomationIpc_ReplayProtection() {
    Ipc::ReplayFilter::Reset();

    Ipc::AutomationCommand cmd;
    cmd.op = Ipc::AutomationOp::Play;

    std::vector<uint8_t> envelope = Ipc::SerializeAutomationEnvelope(cmd);
    TEST_ASSERT(!envelope.empty());

    uint64_t now = GetTickCount64();

    // First receive: Success
    Ipc::AutomationCommand decoded1;
    Ipc::ValidationResult res1 = Ipc::DeserializeAutomationEnvelope(
        envelope.data(), envelope.size(), decoded1, nullptr, true, true, now
    );
    TEST_ASSERT_EQ(res1, Ipc::ValidationResult::Success);

    // Replay with exact same envelope: ReplayDetected
    Ipc::AutomationCommand decoded2;
    Ipc::ValidationResult res2 = Ipc::DeserializeAutomationEnvelope(
        envelope.data(), envelope.size(), decoded2, nullptr, true, true, now
    );
    TEST_ASSERT_EQ(res2, Ipc::ValidationResult::ReplayDetected);

    // Stale message (> 30s old)
    Ipc::AutomationCommand cmdStale;
    cmdStale.op = Ipc::AutomationOp::Pause;
    std::vector<uint8_t> staleEnv = Ipc::SerializeAutomationEnvelope(cmdStale);
    Ipc::EnvelopeHeader* hStale = reinterpret_cast<Ipc::EnvelopeHeader*>(staleEnv.data());
    hStale->timestamp = now - 35000;

    Ipc::AutomationCommand decodedStale;
    Ipc::ValidationResult resStale = Ipc::DeserializeAutomationEnvelope(
        staleEnv.data(), staleEnv.size(), decodedStale, nullptr, true, true, now
    );
    TEST_ASSERT_EQ(resStale, Ipc::ValidationResult::ReplayDetected);

    // Future message (> 5s ahead)
    Ipc::AutomationCommand cmdFuture;
    cmdFuture.op = Ipc::AutomationOp::Stop;
    std::vector<uint8_t> futureEnv = Ipc::SerializeAutomationEnvelope(cmdFuture);
    Ipc::EnvelopeHeader* hFuture = reinterpret_cast<Ipc::EnvelopeHeader*>(futureEnv.data());
    hFuture->timestamp = now + 10000;

    Ipc::AutomationCommand decodedFuture;
    Ipc::ValidationResult resFuture = Ipc::DeserializeAutomationEnvelope(
        futureEnv.data(), futureEnv.size(), decodedFuture, nullptr, true, true, now
    );
    TEST_ASSERT_EQ(resFuture, Ipc::ValidationResult::ReplayDetected);

    Ipc::ReplayFilter::Reset();
}
static void step_AutomationIpc_StatusResponseRoundTrip() {
    Ipc::AutomationStatus status;
    status.state = Ipc::AutomationPlaybackState::Playing;
    status.positionSec = 142.5;
    status.durationSec = 3600.0;
    status.volume = 85.0;
    status.isMuted = false;
    status.isLoop = true;
    status.isFullscreen = true;
    status.currentFilePath = L"C:\\Media\\FeatureFilm.mkv";

    std::vector<uint8_t> responseEnv = Ipc::SerializeAutomationResponse(status, 0);
    TEST_ASSERT(!responseEnv.empty());

    Ipc::AutomationStatus decodedStatus;
    uint32_t statusResult = 1;
    Ipc::EnvelopeHeader header = {};
    Ipc::ValidationResult res = Ipc::DeserializeAutomationResponse(
        responseEnv.data(), responseEnv.size(), decodedStatus, &statusResult, &header, true
    );

    TEST_ASSERT_EQ(res, Ipc::ValidationResult::Success);
    TEST_ASSERT_EQ(statusResult, 0u);
    TEST_ASSERT_EQ(header.command, (uint32_t)Ipc::Command::AutomationResponse);
    TEST_ASSERT_EQ(decodedStatus.state, Ipc::AutomationPlaybackState::Playing);
    TEST_ASSERT_EQ(decodedStatus.positionSec, 142.5);
    TEST_ASSERT_EQ(decodedStatus.durationSec, 3600.0);
    TEST_ASSERT_EQ(decodedStatus.volume, 85.0);
    TEST_ASSERT(!decodedStatus.isMuted);
    TEST_ASSERT(decodedStatus.isLoop);
    TEST_ASSERT(decodedStatus.isFullscreen);
    TEST_ASSERT_EQ(decodedStatus.currentFilePath, L"C:\\Media\\FeatureFilm.mkv");
}


TEST_CASE(IpcEnvelope, SerializationBoundsAndTokenAuth) {
    step_IpcEnvelope_RoundTripBasic();
    step_IpcEnvelope_RoundTripUnicodeCharacters();
    step_IpcEnvelope_RoundTripLongPaths();
    step_IpcEnvelope_RejectOversizedPayload();
    step_IpcEnvelope_RejectExcessiveFileCount();
    step_IpcEnvelope_RejectSinglePathTooLong();
    step_IpcEnvelope_RejectInvalidMagic();
    step_IpcEnvelope_RejectUnsupportedVersion();
    step_IpcEnvelope_RejectCorruptedChecksum();
    step_IpcEnvelope_RejectTruncatedPayload();
    step_IpcEnvelope_RejectEmbeddedNulls();
    step_IpcEnvelope_AppPayloadConversion();
    step_IpcEnvelope_SameUserAuthenticationCurrentProcess();
    step_IpcEnvelope_RoundTripWithOptions();
    step_IpcEnvelope_Version1BackwardCompatibility();
}

TEST_CASE(CommandLineParser, FlagPathAndUrlParsingContract) {
    step_CommandLineParser_ParseValidFlagsAll();
    step_CommandLineParser_ParseTimeFormats();
    step_CommandLineParser_RejectMalformedTime();
    step_CommandLineParser_ParseVolume();
    step_CommandLineParser_RejectMalformedVolume();
    step_CommandLineParser_ParseBooleanFlags();
    step_CommandLineParser_ValidatedUrlsAndPaths();
    step_CommandLineParser_OrderIndependence();
    step_CommandLineParser_RejectUnknownFlags();
    step_CommandLineParser_BoundsChecks();
}

TEST_CASE(AutomationIpc, DispatchAndReplayProtection) {
    step_AutomationIpc_BasicOperationsRoundTrip();
    step_AutomationIpc_SeekOperations();
    step_AutomationIpc_VolumeOperations();
    step_AutomationIpc_OpenAndEnqueueOperations();
    step_AutomationIpc_RejectDisallowedOperations();
    step_AutomationIpc_RejectInvalidArguments();
    step_AutomationIpc_RejectSecurityViolations();
    step_AutomationIpc_ReplayProtection();
    step_AutomationIpc_StatusResponseRoundTrip();
    auto replayFn = &Ipc::ReplayFilter::ValidateAndRecord; TEST_ASSERT(replayFn != nullptr);
}

} // namespace Aperture::Test
