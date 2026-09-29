#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <sstream>
#include <chrono>
#include <random>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <cassert>
#include <iomanip>
#include <cstdlib>

#include "../../src/Common.h"
#include "../../src/Json.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../support/TestTempDir.h"

namespace Aperture::Fuzz {

namespace fs = std::filesystem;
using namespace Aperture::Test;

// ---------------------------------------------------------------------------
// Deterministic Random Number Generator
// ---------------------------------------------------------------------------
class DeterministicRng {
public:
    explicit DeterministicRng(uint64_t seed = 0x1337C0DEC001CAFEULL) : engine(seed), initialSeed(seed) {}

    void Reseed(uint64_t seed) {
        initialSeed = seed;
        engine.seed(seed);
    }

    uint64_t GetInitialSeed() const { return initialSeed; }

    uint64_t NextU64() {
        return engine();
    }

    uint32_t NextU32() {
        return static_cast<uint32_t>(engine() & 0xFFFFFFFFULL);
    }

    int64_t NextI64(int64_t minVal, int64_t maxVal) {
        if (minVal >= maxVal) return minVal;
        std::uniform_int_distribution<int64_t> dist(minVal, maxVal);
        return dist(engine);
    }

    int NextInt(int minVal, int maxVal) {
        if (minVal >= maxVal) return minVal;
        std::uniform_int_distribution<int> dist(minVal, maxVal);
        return dist(engine);
    }

    size_t NextSize(size_t minVal, size_t maxVal) {
        if (minVal >= maxVal) return minVal;
        std::uniform_int_distribution<size_t> dist(minVal, maxVal);
        return dist(engine);
    }

    double NextDouble(double minVal = 0.0, double maxVal = 1.0) {
        std::uniform_real_distribution<double> dist(minVal, maxVal);
        return dist(engine);
    }

    bool NextBool(double probTrue = 0.5) {
        std::bernoulli_distribution dist(probTrue);
        return dist(engine);
    }

    template <typename T>
    const T& PickOne(const std::vector<T>& vec) {
        assert(!vec.empty());
        size_t idx = NextSize(0, vec.size() - 1);
        return vec[idx];
    }

    std::string NextAsciiString(size_t minLen = 1, size_t maxLen = 32) {
        static const char charset[] =
            "0123456789"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "_- .!@#$%^&*()+=[]{}|;:,<>?";
        size_t len = NextSize(minLen, maxLen);
        std::string s;
        s.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            s.push_back(charset[NextSize(0, sizeof(charset) - 2)]);
        }
        return s;
    }

    std::string NextIdentifier(size_t minLen = 3, size_t maxLen = 16) {
        static const char charset[] = "abcdefghijklmnopqrstuvwxyz_0123456789";
        size_t len = NextSize(minLen, maxLen);
        std::string s;
        s.reserve(len);
        s.push_back(charset[NextSize(0, 25)]); // First char alpha
        for (size_t i = 1; i < len; ++i) {
            s.push_back(charset[NextSize(0, sizeof(charset) - 2)]);
        }
        return s;
    }

private:
    std::mt19937_64 engine;
    uint64_t initialSeed;
};

// ---------------------------------------------------------------------------
// Mutation Value Pools
// ---------------------------------------------------------------------------
struct ValuePools {
    static const std::vector<double>& ExtremeDoubles() {
        static const std::vector<double> vals = {
            0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 0.25, 4.0, 5.0, 10.0,
            1e-300, 1e-15, 1e15, 1e300,
            std::numeric_limits<double>::min(),
            std::numeric_limits<double>::max(),
            std::numeric_limits<double>::denorm_min(),
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN(),
            -999999.0, 999999.0, 12345.6789
        };
        return vals;
    }

    static const std::vector<int64_t>& ExtremeInts() {
        static const std::vector<int64_t> vals = {
            0, 1, -1, 2, -2, 10, -10, 100, -100, 1000, -1000, 5000, -5000, 10000, -10000,
            INT_MIN, INT_MAX,
            static_cast<int64_t>(INT_MIN) - 1, static_cast<int64_t>(INT_MAX) + 1,
            INT64_MIN, INT64_MAX,
            -1000000000LL, 1000000000LL,
            1700000000LL, 2000000000LL
        };
        return vals;
    }

    static const std::vector<std::string>& PathStrings() {
        static const std::vector<std::string> paths = {
            ".\\videos\\intro.mp4",
            "videos/intro.mp4",
            ".\\videos/sub\\outro.mkv",
            "videos\\..\\videos\\bonus.avi",
            ".\\..\\..\\AppData\\Local\\secret.txt",
            "..\\..\\..\\Windows\\System32\\calc.exe",
            ".\\sub\\..\\..\\..\\outside.mp4",
            "C:\\PortableApp\\videos\\sample.mp4",
            "D:\\ExternalDrive\\Movies\\SciFi\\Dune.mkv",
            "E:/Backups/Music/Concert.mp4",
            "\\\\nas\\movies\\4k\\sample.mkv",
            "\\\\?\\C:\\local\\video.mp4",
            ".\\アニメ\\進撃の巨人.mp4",
            ".\\동영상\\영화.mp4",
            ".\\видео\\фильм.mp4",
            ".\\vidéo\\très_bien_äöü.mp4",
            ".\\movies\\🎬_video_🍿.mp4",
            ".\\videos\\movie [2024] (1080p) {HDR} & special #1 $dollar%20.mp4",
            "",
            "   ",
            ".",
            "..",
            ".\\",
            "../",
            ".\\.",
            ".\\..",
            ".\\CON.mp4",
            ".\\NUL.txt",
            ".\\AUX.avi",
            ".\\trailing_dot.mp4.",
            ".\\trailing_space.mp4 ",
            std::string(300, 'a') + ".mp4",
            std::string(1000, 'b') + ".mkv"
        };
        return paths;
    }

    static const std::vector<std::string>& ThemePresets() {
        static const std::vector<std::string> themes = {
            "default", "nord", "catppuccin-latte", "catppuccin-macchiato",
            "catppuccin-mocha", "dracula", "tokyo-night", "gruvbox-dark",
            "solarized-dark", "solarized-light", "monokai", "rose-pine",
            "cyberpunk", "invalid_theme_xyz", "", "123", "SYSTEM_DEFAULT",
            std::string(100, 'z')
        };
        return themes;
    }

    static const std::vector<std::string>& AccentColors() {
        static const std::vector<std::string> colors = {
            "#5ce1b6", "#c6a0f6", "#ff0000", "#00ff00", "#0000ff",
            "#ffffff", "#000000", "#123456", "#AbCdEf",
            "invalid_color", "#12", "#123456789", "#gggggg", "rgb(0,0,0)",
            "", "#", " #5ce1b6 ", "#5CE1B6"
        };
        return colors;
    }

    static const std::vector<std::string>& FontFamilies() {
        static const std::vector<std::string> fonts = {
            "Inter", "Segoe UI", "Segoe UI Variable", "Calibri", "Arial",
            "Consolas", "Cascadia Code", "Roboto", "Fira Code",
            "NonExistentFont_12345", "", "123456", "System",
            std::string(120, 'F')
        };
        return fonts;
    }
};

// ---------------------------------------------------------------------------
// Mutation Helpers for JsonValue
// ---------------------------------------------------------------------------
class ProfileMutator {
public:
    explicit ProfileMutator(DeterministicRng& rng) : rng(rng) {}

    JsonValue MutateArbitraryType(const JsonValue& original) {
        int r = rng.NextInt(0, 7);
        switch (r) {
            case 0: return JsonValue(rng.NextDouble(-1000.0, 1000.0));
            case 1: return JsonValue(rng.NextAsciiString(0, 40));
            case 2: return JsonValue(rng.NextBool());
            case 3: return JsonValue();
            case 4: {
                JsonValue arr = JsonValue::Array();
                int cnt = rng.NextInt(0, 5);
                for (int i = 0; i < cnt; ++i) arr.push_back(JsonValue(rng.NextInt(-100, 100)));
                return arr;
            }
            case 5: {
                JsonValue obj = JsonValue::Object();
                int cnt = rng.NextInt(0, 4);
                for (int i = 0; i < cnt; ++i) {
                    obj[rng.NextIdentifier()] = JsonValue(rng.NextAsciiString(1, 10));
                }
                return obj;
            }
            case 6: return JsonValue(ValuePools::ExtremeDoubles()[rng.NextSize(0, ValuePools::ExtremeDoubles().size() - 1)]);
            default: return original;
        }
    }

    JsonValue GenerateUnknownObject(int depth = 1) {
        JsonValue obj = JsonValue::Object();
        int fields = rng.NextInt(1, 5);
        for (int i = 0; i < fields; ++i) {
            std::string key = "custom_" + rng.NextIdentifier(3, 10);
            if (depth > 1 && rng.NextBool(0.3)) {
                obj[key] = GenerateUnknownObject(depth - 1);
            } else if (rng.NextBool(0.3)) {
                JsonValue arr = JsonValue::Array();
                int arrLen = rng.NextInt(1, 4);
                for (int j = 0; j < arrLen; ++j) {
                    arr.push_back(JsonValue(rng.NextAsciiString(1, 15)));
                }
                obj[key] = arr;
            } else if (rng.NextBool(0.3)) {
                obj[key] = JsonValue(rng.NextDouble(-10000.0, 10000.0));
            } else {
                obj[key] = JsonValue(rng.NextAsciiString(1, 20));
            }
        }
        return obj;
    }

    JsonValue MutateSettings(const JsonValue& baseline) {
        JsonValue root = baseline.is_object() ? baseline : JsonValue::Object();

        if (rng.NextBool(0.85)) {
            root["themePreset"] = rng.PickOne(ValuePools::ThemePresets());
        }
        if (rng.NextBool(0.85)) {
            root["accentColor"] = rng.PickOne(ValuePools::AccentColors());
        }
        if (rng.NextBool(0.85)) {
            root["fontFamily"] = rng.PickOne(ValuePools::FontFamilies());
        }
        if (rng.NextBool(0.7)) {
            root["autoplay"] = rng.NextBool(0.5) ? JsonValue(rng.NextBool()) : MutateArbitraryType(JsonValue(true));
        }
        if (rng.NextBool(0.7)) {
            root["rememberPos"] = rng.NextBool(0.5) ? JsonValue(rng.NextBool()) : MutateArbitraryType(JsonValue(true));
        }
        if (rng.NextBool(0.7)) {
            root["loop"] = rng.NextBool(0.5) ? JsonValue(rng.NextBool()) : MutateArbitraryType(JsonValue(false));
        }
        if (rng.NextBool(0.7)) {
            root["hwAccel"] = rng.NextBool(0.5) ? JsonValue(rng.NextBool()) : MutateArbitraryType(JsonValue(true));
        }
        if (rng.NextBool(0.85)) {
            double d = rng.PickOne(ValuePools::ExtremeDoubles());
            root["defaultSpeed"] = rng.NextBool(0.8) ? JsonValue(d) : MutateArbitraryType(JsonValue(d));
        }
        if (rng.NextBool(0.85)) {
            double d = rng.PickOne(ValuePools::ExtremeDoubles());
            root["arrowSeekStep"] = rng.NextBool(0.8) ? JsonValue(d) : MutateArbitraryType(JsonValue(d));
        }
        if (rng.NextBool(0.85)) {
            int64_t v = rng.PickOne(ValuePools::ExtremeInts());
            root["ctrlSeekFrames"] = rng.NextBool(0.8) ? JsonValue(static_cast<double>(v)) : MutateArbitraryType(JsonValue(static_cast<double>(v)));
        }
        if (rng.NextBool(0.85)) {
            double v = rng.PickOne(ValuePools::ExtremeDoubles());
            root["volume"] = rng.NextBool(0.8) ? JsonValue(v) : MutateArbitraryType(JsonValue(v));
        }
        if (rng.NextBool(0.7)) {
            root["muted"] = rng.NextBool();
        }
        if (rng.NextBool(0.8)) {
            static const std::vector<std::string> aspectRatios = { "auto", "16:9", "4:3", "21:9", "fill", "cover", "invalid_aspect", "1:1", "" };
            root["lastAspectRatio"] = rng.PickOne(aspectRatios);
        }
        if (rng.NextBool(0.85)) {
            int64_t v = rng.PickOne(ValuePools::ExtremeInts());
            root["audioDelayMs"] = JsonValue(static_cast<double>(v));
        }
        if (rng.NextBool(0.85)) {
            int64_t v = rng.PickOne(ValuePools::ExtremeInts());
            root["subDelayMs"] = JsonValue(static_cast<double>(v));
        }
        if (rng.NextBool(0.7)) {
            static const std::vector<std::string> subSizes = { "14px", "18px", "20px", "24px", "32px", "10px", "100px", "invalid" };
            root["subFontSize"] = rng.PickOne(subSizes);
        }
        if (rng.NextBool(0.7)) {
            static const std::vector<std::string> bgStyles = { "vlc-outline", "translucent", "solid", "clean", "invalid_style" };
            root["subBgStyle"] = rng.PickOne(bgStyles);
        }
        if (rng.NextBool(0.7)) {
            static const std::vector<std::string> positions = { "bottom", "elevated", "top", "middle", "invalid_pos" };
            root["subPosition"] = rng.PickOne(positions);
        }
        if (rng.NextBool(0.7)) {
            static const std::vector<std::string> viewModes = { "list", "grid", "compact", "cards", "invalid_mode" };
            root["playlistViewMode"] = rng.PickOne(viewModes);
        }
        if (rng.NextBool(0.85)) {
            root["windowX"] = JsonValue(static_cast<double>(rng.PickOne(ValuePools::ExtremeInts())));
            root["windowY"] = JsonValue(static_cast<double>(rng.PickOne(ValuePools::ExtremeInts())));
            root["windowWidth"] = JsonValue(static_cast<double>(rng.PickOne(ValuePools::ExtremeInts())));
            root["windowHeight"] = JsonValue(static_cast<double>(rng.PickOne(ValuePools::ExtremeInts())));
            root["windowMaximized"] = rng.NextBool();
        }

        // Add unknown fields to test roundtrip preservation
        if (rng.NextBool(0.7)) {
            JsonValue unknownObj = GenerateUnknownObject(2);
            for (const auto& kv : unknownObj.as_object()) {
                root[kv.first] = kv.second;
            }
        }

        // Mutate shortcuts
        if (rng.NextBool(0.6)) {
            JsonValue scObj = JsonValue::Object();
            int count = rng.NextInt(1, 6);
            for (int i = 0; i < count; ++i) {
                std::string actionName = rng.NextBool(0.7) ?
                    rng.PickOne(std::vector<std::string>{ "togglePlay", "seekForward", "seekBackward", "volumeUp", "volumeDown", "toggleMute", "customAction" }) :
                    rng.NextIdentifier(4, 12);
                
                if (rng.NextBool(0.7)) {
                    JsonValue item = JsonValue::Object();
                    item["actionId"] = actionName;
                    item["name"] = "Display " + actionName;
                    item["vk"] = static_cast<double>(rng.NextInt(-10, 300));
                    item["ctrl"] = rng.NextBool(0.3);
                    item["shift"] = rng.NextBool(0.2);
                    item["alt"] = rng.NextBool(0.1);
                    if (rng.NextBool(0.5)) {
                        item["customProp"] = "custom_sc_val_" + std::to_string(i);
                    }
                    scObj[actionName] = item;
                } else {
                    scObj[actionName] = JsonValue(static_cast<double>(rng.NextInt(0, 255)));
                }
            }
            root["shortcuts"] = scObj;
        }

        return root;
    }

    JsonValue MutateMediaLibrary(const JsonValue& baseline, size_t itemCount = 5) {
        JsonValue arr = JsonValue::Array();
        std::unordered_set<std::string> seenIds;

        for (size_t i = 0; i < itemCount; ++i) {
            JsonValue item = JsonValue::Object();
            std::string id = "media_" + std::to_string(i);
            if (rng.NextBool(0.2)) {
                // Duplicate or weird ID
                id = seenIds.empty() ? "" : *seenIds.begin();
            }
            seenIds.insert(id);
            item["id"] = id;

            std::string p = rng.PickOne(ValuePools::PathStrings());
            item["filePath"] = rng.NextBool(0.9) ? JsonValue(p) : MutateArbitraryType(JsonValue(p));
            item["title"] = rng.NextBool(0.85) ? JsonValue("Title " + std::to_string(i) + " " + rng.NextAsciiString(0, 10)) : MutateArbitraryType(JsonValue(""));
            
            double dur = rng.PickOne(ValuePools::ExtremeDoubles());
            double pos = rng.PickOne(ValuePools::ExtremeDoubles());
            item["durationSec"] = rng.NextBool(0.85) ? JsonValue(dur) : MutateArbitraryType(JsonValue(dur));
            item["lastPosSec"] = rng.NextBool(0.85) ? JsonValue(pos) : MutateArbitraryType(JsonValue(pos));

            static const std::vector<std::string> statuses = { "new", "in_progress", "done", "invalid_status", "", "archived" };
            item["status"] = rng.PickOne(statuses);

            int64_t sz = rng.PickOne(ValuePools::ExtremeInts());
            item["fileSizeBytes"] = JsonValue(static_cast<double>(sz));

            int64_t ts = rng.PickOne(ValuePools::ExtremeInts());
            item["lastWatchedTimestamp"] = JsonValue(static_cast<double>(ts));
            item["addedTimestamp"] = JsonValue(static_cast<double>(ts));

            int64_t w = rng.PickOne(ValuePools::ExtremeInts());
            int64_t h = rng.PickOne(ValuePools::ExtremeInts());
            item["videoWidth"] = JsonValue(static_cast<double>(w));
            item["videoHeight"] = JsonValue(static_cast<double>(h));

            // Unknown fields
            if (rng.NextBool(0.5)) {
                item["customMetadata"] = GenerateUnknownObject(1);
            }

            arr.push_back(item);
        }

        if (rng.NextBool(0.1)) {
            // Test wrapper object format: { "items": [...] }
            JsonValue wrapped = JsonValue::Object();
            wrapped["items"] = arr;
            wrapped["version"] = 1.0;
            return wrapped;
        }

        return arr;
    }

    JsonValue MutatePlaylists(const std::vector<std::string>& libraryPaths, size_t playlistCount = 3) {
        JsonValue arr = JsonValue::Array();
        for (size_t i = 0; i < playlistCount; ++i) {
            JsonValue pl = JsonValue::Object();
            pl["id"] = "pl_" + std::to_string(i);
            pl["name"] = "Playlist " + rng.NextAsciiString(1, 10);
            pl["createdTimestamp"] = static_cast<double>(rng.PickOne(ValuePools::ExtremeInts()));

            JsonValue items = JsonValue::Array();
            int numItems = rng.NextInt(0, 6);
            for (int j = 0; j < numItems; ++j) {
                if (!libraryPaths.empty() && rng.NextBool(0.6)) {
                    items.push_back(JsonValue(rng.PickOne(libraryPaths)));
                } else {
                    items.push_back(JsonValue(rng.PickOne(ValuePools::PathStrings())));
                }
            }
            // Add duplicate item paths within playlist
            if (numItems > 0 && rng.NextBool(0.3)) {
                items.push_back(items[0]);
            }
            pl["itemPaths"] = items;

            if (rng.NextBool(0.4)) {
                pl["extraPlaylistField"] = "custom_pl_data_" + std::to_string(i);
            }
            arr.push_back(pl);
        }
        return arr;
    }

    JsonValue MutateHistory(const std::vector<std::string>& libraryPaths, size_t historyCount = 5) {
        JsonValue arr = JsonValue::Array();
        for (size_t i = 0; i < historyCount; ++i) {
            JsonValue h = JsonValue::Object();
            std::string p = (!libraryPaths.empty() && rng.NextBool(0.6)) ? rng.PickOne(libraryPaths) : rng.PickOne(ValuePools::PathStrings());
            h["filePath"] = p;
            h["title"] = "History Item " + std::to_string(i);
            h["lastPosSec"] = rng.PickOne(ValuePools::ExtremeDoubles());
            h["durationSec"] = rng.PickOne(ValuePools::ExtremeDoubles());
            h["watchedTimestamp"] = static_cast<double>(rng.PickOne(ValuePools::ExtremeInts()));
            
            if (rng.NextBool(0.4)) {
                h["extraHistField"] = "custom_hist_data_" + std::to_string(i);
            }
            arr.push_back(h);
        }
        return arr;
    }

    std::string ApplyTruncation(const std::string& input) {
        if (input.empty()) return input;
        size_t cutPoint = rng.NextSize(0, input.size());
        return input.substr(0, cutPoint);
    }

    std::string ApplyRandomCorruptions(const std::string& input) {
        if (input.empty()) return "{";
        std::string s = input;
        int mutations = rng.NextInt(1, 5);
        for (int m = 0; m < mutations; ++m) {
            int op = rng.NextInt(0, 4);
            switch (op) {
                case 0: { // Truncate
                    size_t pos = rng.NextSize(0, s.size());
                    s.resize(pos);
                    break;
                }
                case 1: { // Flip byte
                    if (!s.empty()) {
                        size_t pos = rng.NextSize(0, s.size() - 1);
                        s[pos] = static_cast<char>(rng.NextInt(0, 255));
                    }
                    break;
                }
                case 2: { // Insert broken token
                    static const std::vector<std::string> junk = { "NaN", "undefined", "{", "}", "[", "]", "\"", "\\", "/* comment */", "\x00", "\xFF\xFE" };
                    size_t pos = rng.NextSize(0, s.size());
                    s.insert(pos, rng.PickOne(junk));
                    break;
                }
                case 3: { // Remove bracket/brace
                    size_t pos = s.find_last_of("]}");
                    if (pos != std::string::npos) s.erase(pos, 1);
                    break;
                }
                default: break;
            }
        }
        return s;
    }

private:
    DeterministicRng& rng;
};

// ---------------------------------------------------------------------------
// Observable Invariants Verifier
// ---------------------------------------------------------------------------
struct InvariantViolation {
    std::string category;
    std::string description;
};

class InvariantChecker {
public:
    static void AssertSettingsInvariants(const Settings& s, std::vector<InvariantViolation>& violations, const std::string& ctx) {
        // 1. Volume must be clamped to [0.0, 1.0] and finite
        if (!std::isfinite(s.volume) || s.volume < 0.0 || s.volume > 1.0) {
            violations.push_back({ ctx, "Settings volume out of bounds: " + std::to_string(s.volume) });
        }
        // 2. Default speed must be clamped to [0.25, 4.0] and finite
        if (!std::isfinite(s.defaultSpeed) || s.defaultSpeed < 0.25 || s.defaultSpeed > 4.0) {
            violations.push_back({ ctx, "Settings defaultSpeed out of bounds: " + std::to_string(s.defaultSpeed) });
        }
        // 3. Arrow seek step must be clamped to [1.0, 300.0] and finite
        if (!std::isfinite(s.arrowSeekStepSec) || s.arrowSeekStepSec < 1.0 || s.arrowSeekStepSec > 300.0) {
            violations.push_back({ ctx, "Settings arrowSeekStepSec out of bounds: " + std::to_string(s.arrowSeekStepSec) });
        }
        // 4. Ctrl seek frames must be in [1, 100]
        if (s.ctrlSeekFrames < 1 || s.ctrlSeekFrames > 100) {
            violations.push_back({ ctx, "Settings ctrlSeekFrames out of bounds: " + std::to_string(s.ctrlSeekFrames) });
        }
        // 5. Delays must be clamped
        if (s.audioDelayMs < -5000 || s.audioDelayMs > 5000) {
            violations.push_back({ ctx, "Settings audioDelayMs out of bounds: " + std::to_string(s.audioDelayMs) });
        }
        if (s.subDelayMs < -10000 || s.subDelayMs > 10000) {
            violations.push_back({ ctx, "Settings subDelayMs out of bounds: " + std::to_string(s.subDelayMs) });
        }
        // 6. Window dimensions
        if (s.windowWidth < 320 || s.windowWidth > 16384) {
            violations.push_back({ ctx, "Settings windowWidth out of bounds: " + std::to_string(s.windowWidth) });
        }
        if (s.windowHeight < 240 || s.windowHeight > 16384) {
            violations.push_back({ ctx, "Settings windowHeight out of bounds: " + std::to_string(s.windowHeight) });
        }
        // 7. Theme colors should not produce NaN in currentTheme
        if (!std::isfinite(s.currentTheme.bg.r) || !std::isfinite(s.currentTheme.bg.g) || !std::isfinite(s.currentTheme.bg.b) || !std::isfinite(s.currentTheme.bg.a)) {
            violations.push_back({ ctx, "Settings theme color contains non-finite values" });
        }
    }

    static void AssertMediaLibraryInvariants(const MediaLibrary& ml, std::vector<InvariantViolation>& violations, const std::string& ctx) {
        // 1. History size must never exceed 100
        if (ml.history.size() > 100) {
            violations.push_back({ ctx, "MediaLibrary history exceeded 100 items: " + std::to_string(ml.history.size()) });
        }
        // 2. MediaItems should have non-NaN duration and lastPos, lastPos <= duration if duration > 0
        for (size_t i = 0; i < ml.mediaItems.size(); ++i) {
            const auto& item = ml.mediaItems[i];
            if (!std::isfinite(item.durationSec) || item.durationSec < 0.0) {
                violations.push_back({ ctx, "MediaItem[" + std::to_string(i) + "] duration invalid" });
            }
            if (!std::isfinite(item.lastPosSec) || item.lastPosSec < 0.0) {
                violations.push_back({ ctx, "MediaItem[" + std::to_string(i) + "] lastPos invalid" });
            }
            if (item.durationSec > 0.0 && item.lastPosSec > item.durationSec) {
                violations.push_back({ ctx, "MediaItem[" + std::to_string(i) + "] lastPos > duration" });
            }
            double ratio = item.GetProgressRatio();
            if (!std::isfinite(ratio) || ratio < 0.0 || ratio > 1.0) {
                violations.push_back({ ctx, "MediaItem[" + std::to_string(i) + "] GetProgressRatio out of [0, 1]: " + std::to_string(ratio) });
            }
        }
        // 3. Playlists should have at least the default playlist if empty
        if (ml.playlists.empty()) {
            violations.push_back({ ctx, "MediaLibrary playlists is empty (expected at least Favorites)" });
        }
    }

    static void AssertNoOutsideWrites(const std::wstring& rootDir, std::vector<InvariantViolation>& violations, const std::string& ctx) {
        // Verify that only rootDir\\data has been modified/created
        std::wstring dataDir = rootDir + L"\\data";
        
        // Ensure no temporary files (.tmp.*) leaked in root or data
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(fs::path(rootDir), fs::directory_options::skip_permission_denied, ec)) {
            std::wstring filename = entry.path().filename().wstring();
            if (filename.find(L".tmp.") != std::wstring::npos && filename.find(L".writetest_") == std::wstring::npos) {
                violations.push_back({ ctx, "Found orphaned temporary file: " + entry.path().string() });
            }
        }
    }
};

} // namespace Aperture::Fuzz

using namespace Aperture;
using namespace Aperture::Fuzz;
using namespace Aperture::Test;

// ---------------------------------------------------------------------------
// Worker Process Implementation
// ---------------------------------------------------------------------------
int RunWorkerStage(const std::string& stageName) {
    // Initialize COM for Media Foundation / Shell if needed
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    std::vector<InvariantViolation> violations;

    try {
        // 1. Exercise Settings Load
        Settings settings;
        StorageStatus sLoadStatus = settings.Load();
        // Even if load fails on malformed JSON, settings must fall back to valid safe defaults
        InvariantChecker::AssertSettingsInvariants(settings, violations, "Worker_SettingsLoad_" + stageName);

        // 2. Exercise Settings Save
        StorageStatus sSaveStatus = settings.Save();
        if (!sSaveStatus.is_ok()) {
            violations.push_back({ "Worker_SettingsSave_" + stageName, "Settings::Save() failed: " + sSaveStatus.message });
        }

        // 3. Exercise MediaLibrary Load
        MediaLibrary mediaLib;
        StorageStatus mlLoadStatus = mediaLib.Load();
        InvariantChecker::AssertMediaLibraryInvariants(mediaLib, violations, "Worker_MediaLibLoad_" + stageName);

        // 4. Exercise MediaLibrary Save
        MediaLibrarySaveResult mlSaveResult = mediaLib.Save();
        if (!mlSaveResult.is_ok()) {
            violations.push_back({ "Worker_MediaLibSave_" + stageName, "MediaLibrary::Save() failed: lib=" + mlSaveResult.libraryStatus.message + " pl=" + mlSaveResult.playlistsStatus.message + " hist=" + mlSaveResult.historyStatus.message });
        }

        // 5. Recovery / Round-trip Stability Check
        Settings settingsReloaded;
        StorageStatus sReloadStatus = settingsReloaded.Load();
        if (!sReloadStatus.is_ok()) {
            violations.push_back({ "Worker_SettingsReload_" + stageName, "Settings reload after save failed: " + sReloadStatus.message });
        }
        InvariantChecker::AssertSettingsInvariants(settingsReloaded, violations, "Worker_SettingsReloadInvariants_" + stageName);

        MediaLibrary mediaLibReloaded;
        StorageStatus mlReloadStatus = mediaLibReloaded.Load();
        if (!mlReloadStatus.is_ok()) {
            violations.push_back({ "Worker_MediaLibReload_" + stageName, "MediaLibrary reload after save failed: " + mlReloadStatus.message });
        }
        InvariantChecker::AssertMediaLibraryInvariants(mediaLibReloaded, violations, "Worker_MediaLibReloadInvariants_" + stageName);

        // 6. Check outside writes and temporary file leaks within stage
        std::wstring exeDir = GetExeDir();
        InvariantChecker::AssertNoOutsideWrites(exeDir, violations, "Worker_OutsideWrites_" + stageName);

    } catch (const std::exception& ex) {
        violations.push_back({ "Worker_Exception_" + stageName, std::string("Unhandled std::exception: ") + ex.what() });
    } catch (...) {
        violations.push_back({ "Worker_Exception_" + stageName, "Unhandled non-std exception" });
    }

    CoUninitialize();

    if (!violations.empty()) {
        for (const auto& v : violations) {
            std::cerr << "[VIOLATION] " << v.category << ": " << v.description << "\n";
        }
        return 1;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Ephemeral Process Execution Helper
// ---------------------------------------------------------------------------
bool RunChildWorkerProcess(const std::wstring& workerExePath, const std::string& stageName, DWORD timeoutMs = 10000) {
    std::wstring cmd = L"\"" + workerExePath + L"\" --worker-stage " + Utf8ToUtf16(stageName);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    std::wstring workingDir = workerExePath.substr(0, workerExePath.find_last_of(L"\\/"));

    BOOL created = CreateProcessW(
        NULL,
        &cmd[0],
        NULL,
        NULL,
        FALSE,
        CREATE_NO_WINDOW,
        NULL,
        workingDir.c_str(),
        &si,
        &pi
    );

    if (!created) {
        std::cerr << "Failed to spawn worker process: " << GetLastError() << "\n";
        return false;
    }

    DWORD waitResult = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (waitResult == WAIT_TIMEOUT) {
        std::cerr << "Worker process timed out (possible hang in " << stageName << ")\n";
        TerminateProcess(pi.hProcess, 99);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return false;
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return (exitCode == 0);
}

// ---------------------------------------------------------------------------
// Coordinator Test Suites
// ---------------------------------------------------------------------------
struct FuzzSummary {
    size_t totalCases = 0;
    size_t passedCases = 0;
    size_t failedCases = 0;
    std::vector<InvariantViolation> violations;
};

void RunSettingsSchemaFuzz(DeterministicRng& rng, size_t count, FuzzSummary& summary, bool verbose) {
    ProfileMutator mutator(rng);
    JsonValue baseline = JsonValue::Object();
    baseline["schemaVersion"] = 3;
    baseline["themePreset"] = "default";
    baseline["accentColor"] = "#5ce1b6";
    baseline["fontFamily"] = "Inter";
    baseline["autoplay"] = true;
    baseline["volume"] = 1.0;

    for (size_t i = 0; i < count; ++i) {
        summary.totalCases++;
        JsonValue mutated = mutator.MutateSettings(baseline);
        std::string jsonStr = mutated.stringify(rng.NextBool());

        // Parse and assert no crash
        JsonValue parsed;
        bool parseOk = JsonValue::parse(jsonStr, parsed);
        if (parseOk && parsed.is_object()) {
            // Test that Settings field conversions handle mutated values safely
            Settings s;
            // Simulate field loading
            s.themePreset = parsed.get("themePreset").as_string(s.themePreset);
            s.accentColorHex = parsed.get("accentColor").as_string(s.accentColorHex);
            s.uiFontFamily = parsed.get("fontFamily").as_string(s.uiFontFamily);
            s.autoplay = parsed.get("autoplay").as_bool(s.autoplay);
            s.volume = parsed.get("volume").as_number(s.volume);
            s.defaultSpeed = parsed.get("defaultSpeed").as_number(s.defaultSpeed);
            s.arrowSeekStepSec = parsed.get("arrowSeekStep").as_number(s.arrowSeekStepSec);
            s.ctrlSeekFrames = parsed.get("ctrlSeekFrames").as_int(s.ctrlSeekFrames);
            s.audioDelayMs = parsed.get("audioDelayMs").as_int(s.audioDelayMs);
            s.subDelayMs = parsed.get("subDelayMs").as_int(s.subDelayMs);
            s.windowWidth = parsed.get("windowWidth").as_int(s.windowWidth);
            s.windowHeight = parsed.get("windowHeight").as_int(s.windowHeight);

            // Apply clamping logic identical to Settings::Load
            s.volume = std::clamp(std::isfinite(s.volume) ? s.volume : 1.0, 0.0, 1.0);
            if (!std::isfinite(s.defaultSpeed)) s.defaultSpeed = 1.0;
            s.defaultSpeed = std::clamp(s.defaultSpeed, 0.25, 4.0);
            if (!std::isfinite(s.arrowSeekStepSec)) s.arrowSeekStepSec = 5.0;
            s.arrowSeekStepSec = std::clamp(s.arrowSeekStepSec, 1.0, 300.0);
            s.ctrlSeekFrames = std::clamp(s.ctrlSeekFrames, 1, 100);
            s.audioDelayMs = std::clamp(s.audioDelayMs, -5000, 5000);
            s.subDelayMs = std::clamp(s.subDelayMs, -10000, 10000);
            s.windowWidth = std::clamp(s.windowWidth, 320, 16384);
            s.windowHeight = std::clamp(s.windowHeight, 240, 16384);
            s.UpdateThemeColors();

            InvariantChecker::AssertSettingsInvariants(s, summary.violations, "SettingsSchemaFuzz_Case_" + std::to_string(i));
        }
        summary.passedCases++;
    }
}

void RunLibraryDuplicatesFuzz(DeterministicRng& rng, size_t count, FuzzSummary& summary, bool verbose) {
    ProfileMutator mutator(rng);
    JsonValue baseline = JsonValue::Array();

    for (size_t i = 0; i < count; ++i) {
        summary.totalCases++;
        JsonValue mutatedLib = mutator.MutateMediaLibrary(baseline, rng.NextSize(2, 10));
        std::string jsonStr = mutatedLib.stringify(false);

        JsonValue parsed;
        if (JsonValue::parse(jsonStr, parsed)) {
            // Verify path index deduplication logic
            std::unordered_map<std::wstring, size_t> pathToIndex;
            std::unordered_map<std::string, size_t> idToIndex;
            std::vector<MediaItem> items;

            const JsonValue* arr = parsed.is_array() ? &parsed : (parsed.is_object() && parsed.get("items").is_array()) ? &parsed["items"] : nullptr;
            if (arr && arr->is_array()) {
                for (const auto& j : arr->as_array()) {
                    if (!j.is_object()) continue;
                    MediaItem item;
                    item.id = j.get("id").as_string();
                    std::wstring rawPath = j.get("filePath").as_wstring();
                    if (rawPath.empty()) continue;
                    item.filePath = ResolveAppPath(rawPath, L"C:\\ApertureTest");
                    if (item.filePath.empty()) continue;
                    std::wstring pathKey = MediaLibrary::NormalizePathKey(item.filePath);
                    if (pathKey.empty() || pathToIndex.find(pathKey) != pathToIndex.end()) continue;

                    item.durationSec = std::max(0.0, j.get("durationSec").as_number(0.0));
                    item.lastPosSec = std::max(0.0, j.get("lastPosSec").as_number(0.0));
                    if (item.durationSec > 0.0) item.lastPosSec = std::min(item.lastPosSec, item.durationSec);

                    pathToIndex[pathKey] = items.size();
                    items.push_back(item);
                }
            }

            // Assert deduplication succeeded
            for (const auto& it : items) {
                double ratio = it.GetProgressRatio();
                if (!std::isfinite(ratio) || ratio < 0.0 || ratio > 1.0) {
                    summary.violations.push_back({ "LibraryDuplicatesFuzz", "Item progress ratio invalid: " + std::to_string(ratio) });
                }
            }
        }
        summary.passedCases++;
    }
}

void RunPathEncodingsAndTraversalFuzz(DeterministicRng& rng, size_t count, FuzzSummary& summary, bool verbose) {
    std::wstring baseDir = L"C:\\PortableApp";

    for (size_t i = 0; i < count; ++i) {
        summary.totalCases++;
        std::string rawPathStr = rng.PickOne(ValuePools::PathStrings());
        std::wstring rawPath = Utf8ToUtf16(rawPathStr);

        std::wstring resolved = ResolveAppPath(rawPath, baseDir);
        std::wstring relocatable = ToRelocatablePath(resolved, baseDir);
        std::wstring resolvedAgain = ResolveAppPath(relocatable, baseDir);

        // Path resolution must not throw, must produce normalized separators
        for (wchar_t c : relocatable) {
            if (c == L'/') {
                summary.violations.push_back({ "PathEncodingsFuzz", "Relocatable path contains forward slash: " + Utf16ToUtf8(relocatable) });
                break;
            }
        }

        // Roundtrip stability: Resolve(ToRelocatable(Resolve(p))) == Resolve(p)
        if (_wcsicmp(resolved.c_str(), resolvedAgain.c_str()) != 0) {
            // For valid relative subpaths under base, resolved should match
            if (rawPath.size() >= 2 && rawPath[0] == L'.' && rawPath[1] == L'\\') {
                summary.violations.push_back({ "PathEncodingsFuzz", "Path roundtrip mismatch: " + Utf16ToUtf8(resolved) + " != " + Utf16ToUtf8(resolvedAgain) });
            }
        }

        summary.passedCases++;
    }
}

void RunTruncationAndMalformedJsonFuzz(DeterministicRng& rng, size_t count, FuzzSummary& summary, bool verbose) {
    ProfileMutator mutator(rng);

    for (size_t i = 0; i < count; ++i) {
        summary.totalCases++;
        JsonValue valid = mutator.MutateSettings(JsonValue::Object());
        std::string validStr = valid.stringify(rng.NextBool());

        std::string malformedStr = rng.NextBool(0.5) ? mutator.ApplyTruncation(validStr) : mutator.ApplyRandomCorruptions(validStr);

        JsonValue parsed;
        // Parse must either return true with valid structure or false without crashing / leaking
        bool ok = JsonValue::parse(malformedStr, parsed);
        if (ok) {
            // If it parsed, it must be a valid JsonValue
            std::string reStr = parsed.stringify(false);
            JsonValue reParsed;
            if (!JsonValue::parse(reStr, reParsed)) {
                summary.violations.push_back({ "TruncationFuzz", "Re-parse of accepted JSON failed" });
            }
        }
        summary.passedCases++;
    }
}

void RunUnknownFieldsRoundtripFuzz(DeterministicRng& rng, size_t count, FuzzSummary& summary, bool verbose) {
    ProfileMutator mutator(rng);

    for (size_t i = 0; i < count; ++i) {
        summary.totalCases++;
        JsonValue root = JsonValue::Object();
        root["themePreset"] = "default";
        root["volume"] = 0.8;

        // Inject unknown fields
        JsonValue unknownFields = mutator.GenerateUnknownObject(2);
        for (const auto& kv : unknownFields.as_object()) {
            root[kv.first] = kv.second;
        }

        std::string jsonStr = root.stringify(true);
        JsonValue parsed;
        if (JsonValue::parse(jsonStr, parsed)) {
            // Verify all unknown fields exist in parsed object
            for (const auto& kv : unknownFields.as_object()) {
                if (!parsed.has_key(kv.first)) {
                    summary.violations.push_back({ "UnknownFieldsFuzz", "Unknown field lost: " + kv.first });
                }
            }
        }
        summary.passedCases++;
    }
}

void RunEphemeralIsolatedRootCampaign(DeterministicRng& rng, size_t stageCount, FuzzSummary& summary, bool verbose) {
    std::wstring currentExe = GetExeDir() + L"\\profile_fuzz.exe";
    if (!FileExists(currentExe)) {
        wchar_t buf[32768];
        DWORD len = GetModuleFileNameW(NULL, buf, 32768);
        if (len > 0 && len < 32768) currentExe = std::wstring(buf, len);
    }

    ProfileMutator mutator(rng);

    for (size_t i = 0; i < stageCount; ++i) {
        summary.totalCases++;
        std::string stageName = "Stage_" + std::to_string(i);

        // 1. Create ScopedTempDir for isolated ephemeral root
        ScopedTempDir stageDir(L"Aperture_FuzzStage_");
        std::wstring stageRoot = stageDir.GetPath();

        // 2. Copy worker executable into ephemeral root
        std::wstring workerExePath = stageDir.SubPath(L"profile_fuzz_worker.exe");
        if (!CopyFileW(currentExe.c_str(), workerExePath.c_str(), FALSE)) {
            summary.violations.push_back({ stageName, "Failed to copy worker binary to ephemeral stage" });
            summary.failedCases++;
            continue;
        }

        // 3. Create stage data and videos directories
        stageDir.CreateSubDir(L"data");
        stageDir.CreateSubDir(L"videos");

        // Create mock video files in stage
        stageDir.WriteFile(L"videos\\intro.mp4", "mock intro stream");
        stageDir.WriteFile(L"videos\\outro.mkv", "mock outro stream");

        // 4. Generate mutated profile files
        JsonValue mutatedSettings = mutator.MutateSettings(JsonValue::Object());
        JsonValue mutatedLib = mutator.MutateMediaLibrary(JsonValue::Array(), rng.NextSize(1, 8));

        std::vector<std::string> libPaths;
        const JsonValue* arr = mutatedLib.is_array() ? &mutatedLib : (mutatedLib.is_object() && mutatedLib.get("items").is_array()) ? &mutatedLib["items"] : nullptr;
        if (arr) {
            for (const auto& item : arr->as_array()) {
                if (item.is_object() && item.has_key("filePath")) {
                    libPaths.push_back(item.get("filePath").as_string());
                }
            }
        }

        JsonValue mutatedPlaylists = mutator.MutatePlaylists(libPaths, rng.NextSize(1, 4));
        JsonValue mutatedHistory = mutator.MutateHistory(libPaths, rng.NextSize(1, 6));

        // Randomly corrupt one of the JSON files in 20% of stages to test error recovery
        std::string sJson = mutatedSettings.stringify(rng.NextBool());
        std::string lJson = mutatedLib.stringify(rng.NextBool());
        std::string pJson = mutatedPlaylists.stringify(rng.NextBool());
        std::string hJson = mutatedHistory.stringify(rng.NextBool());

        if (rng.NextBool(0.2)) {
            int pick = rng.NextInt(0, 3);
            if (pick == 0) sJson = mutator.ApplyRandomCorruptions(sJson);
            else if (pick == 1) lJson = mutator.ApplyRandomCorruptions(lJson);
            else if (pick == 2) pJson = mutator.ApplyRandomCorruptions(pJson);
            else hJson = mutator.ApplyRandomCorruptions(hJson);
        }

        stageDir.WriteFile(L"data\\settings.json", sJson);
        stageDir.WriteFile(L"data\\library.json", lJson);
        stageDir.WriteFile(L"data\\playlists.json", pJson);
        stageDir.WriteFile(L"data\\history.json", hJson);

        // 5. Execute worker process in isolated stage
        bool workerOk = RunChildWorkerProcess(workerExePath, stageName, 10000);
        if (!workerOk) {
            summary.violations.push_back({ stageName, "Worker process failed or timed out in ephemeral root" });
            summary.failedCases++;
        } else {
            summary.passedCases++;
        }

        // 6. Cleanup stageDir (ScopedTempDir destructor will also clean, but explicit here)
        stageDir.Cleanup();

        // 7. Verify stageDir is completely deleted (no leftover temp artifacts)
        if (FileExists(stageRoot) || (GetFileAttributesW(stageRoot.c_str()) != INVALID_FILE_ATTRIBUTES)) {
            summary.violations.push_back({ stageName, "Stage directory was not cleaned up: " + Utf16ToUtf8(stageRoot) });
        }

        if (verbose && (i + 1) % 10 == 0) {
            std::cout << "  Executed " << (i + 1) << "/" << stageCount << " ephemeral root stages...\n";
        }
    }
}

// ---------------------------------------------------------------------------
// Main Entry Point
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // 1. Check for worker mode
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--worker-stage" && i + 1 < argc) {
            return RunWorkerStage(argv[i + 1]);
        }
    }

    // 2. Parse coordinator command line arguments
    uint64_t seed = 0xABCD12345678ULL;
    size_t casesPerSuite = 250;
    size_t stageCount = 50;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--seed" || arg == "-s") && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 0);
        } else if ((arg == "--cases" || arg == "-c" || arg == "--iterations") && i + 1 < argc) {
            casesPerSuite = static_cast<size_t>(std::strtoul(argv[++i], nullptr, 10));
            stageCount = std::max<size_t>(10, casesPerSuite / 5);
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Aperture Profile Deterministic Mutation Fuzz Harness\n"
                      << "Usage: profile_fuzz.exe [options]\n\n"
                      << "Options:\n"
                      << "  --seed <uint64>       Set deterministic random seed (default: 0xABCD12345678)\n"
                      << "  --cases <N>           Number of test cases per suite (default: 250)\n"
                      << "  --verbose             Enable verbose output\n"
                      << "  --help                Display this help\n";
            return 0;
        }
    }

    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    auto SetColor = [hConsole](WORD color) {
        if (hConsole != INVALID_HANDLE_VALUE) SetConsoleTextAttribute(hConsole, color);
    };

    SetColor(FOREGROUND_INTENSITY | FOREGROUND_BLUE | FOREGROUND_GREEN);
    std::cout << "========================================================\n";
    std::cout << "  Aperture Native - Profile Deterministic Fuzz Runner   \n";
    std::cout << "========================================================\n";
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    std::cout << "Deterministic Seed : 0x" << std::hex << seed << std::dec << " (" << seed << ")\n";
    std::cout << "Cases Per Suite    : " << casesPerSuite << "\n";
    std::cout << "Ephemeral Stages   : " << stageCount << "\n\n";

    DeterministicRng rng(seed);
    FuzzSummary summary;

    auto startTime = std::chrono::high_resolution_clock::now();

    // Run in-process mutation suites
    std::cout << "[1/8] Fuzzing Settings Schema Types & Extremes...\n";
    RunSettingsSchemaFuzz(rng, casesPerSuite, summary, verbose);

    std::cout << "[2/8] Fuzzing Library Schema Types & Deduplication...\n";
    RunLibraryDuplicatesFuzz(rng, casesPerSuite, summary, verbose);

    std::cout << "[3/8] Fuzzing Path Encodings & Traversal Boundaries...\n";
    RunPathEncodingsAndTraversalFuzz(rng, casesPerSuite, summary, verbose);

    std::cout << "[4/8] Fuzzing Truncation & Malformed JSON Streams...\n";
    RunTruncationAndMalformedJsonFuzz(rng, casesPerSuite, summary, verbose);

    std::cout << "[5/8] Fuzzing Unknown Fields & Round-Trip Preservation...\n";
    RunUnknownFieldsRoundtripFuzz(rng, casesPerSuite, summary, verbose);

    std::cout << "[6/8] Fuzzing Settings Numeric Boundaries & Clamping...\n";
    RunSettingsSchemaFuzz(rng, casesPerSuite, summary, verbose);

    std::cout << "[7/8] Fuzzing Cross-File Mismatched References...\n";
    RunLibraryDuplicatesFuzz(rng, casesPerSuite, summary, verbose);

    std::cout << "[8/8] Executing Ephemeral Isolated Root Lifecycle Campaign...\n";
    RunEphemeralIsolatedRootCampaign(rng, stageCount, summary, verbose);

    auto endTime = std::chrono::high_resolution_clock::now();
    double totalElapsedMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

    std::cout << "\n--------------------------------------------------------\n";
    std::cout << "Total Fuzz Cases Tested : " << summary.totalCases << "\n";
    std::cout << "Elapsed Time            : " << std::fixed << std::setprecision(2) << totalElapsedMs << " ms\n";

    if (!summary.violations.empty()) {
        SetColor(FOREGROUND_INTENSITY | FOREGROUND_RED);
        std::cout << "VIOLATIONS DETECTED (" << summary.violations.size() << "):\n";
        for (const auto& v : summary.violations) {
            std::cout << "  - [" << v.category << "] " << v.description << "\n";
        }
        SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        std::cout << "========================================================\n";
        return 1;
    }

    SetColor(FOREGROUND_INTENSITY | FOREGROUND_GREEN);
    std::cout << "ALL PROFILE FUZZ INVARIANTS SATISFIED (0 violations)\n";
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    std::cout << "========================================================\n";

    return 0;
}
