#include "ThumbnailCodec.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <chrono>
#include <stdexcept>

using namespace Aperture;
namespace fs = std::filesystem;
struct Frame { std::string name; int width, height; std::vector<uint8_t> pixels; };
void Require(bool value) { if (!value) throw std::runtime_error("Codec benchmark validation failed"); }
std::string FixedJpeg(const Frame& f) {
    std::vector<uint8_t> payload;
    bool opaque = true;
    for (size_t i = 3; i < f.pixels.size(); i += 4) { if (f.pixels[i] != 255) { opaque = false; break; } }
    Require(ThumbnailCodec::EncodeImage(f.pixels.data(), f.width, f.height, opaque, payload));
    ThumbnailDiskHeader header;
    header.version = opaque ? 3 : 2; header.width = f.width; header.height = f.height;
    header.stride = f.width * 4; header.timestamp = 12.5; header.pixelBytes = static_cast<uint32_t>(payload.size());
    std::string out(sizeof(header) + payload.size(), '\0');
    std::memcpy(out.data(), &header, sizeof(header));
    std::memcpy(out.data() + sizeof(header), payload.data(), payload.size());
    return out;
}
Frame Fixture(int width, int height, int kind) {
    const char* names[] = {"solid", "palette-bars", "checkerboard", "subtitle-blocks", "natural-grain", "mixed-flat-photo", "transparent"};
    Frame f{names[kind], width, height, std::vector<uint8_t>(static_cast<size_t>(width) * height * 4)};
    std::mt19937 random(731);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const size_t i = (static_cast<size_t>(y) * width + x) * 4;
        uint8_t b = 32, g = 32, r = 32;
        if (kind == 1) { b = (x * 8 / width & 1) ? 240 : 16; g = (x * 8 / width & 2) ? 240 : 16; r = (x * 8 / width & 4) ? 240 : 16; }
        if (kind == 2) b = g = r = (x + y) % 2 ? 0 : 255;
        if (kind == 3 && y > height * 3 / 4 && (x / 5) % 4 != 0 && (y / 3) % 3 == 0) b = g = r = 240;
        if (kind == 4 || kind == 6 || (kind == 5 && x < width / 4)) {
            const int grain = static_cast<int>(random() % 9) - 4;
            b = static_cast<uint8_t>(std::clamp(x * 210 / width + grain, 0, 255));
            g = static_cast<uint8_t>(std::clamp(y * 190 / height + grain, 0, 255));
            r = static_cast<uint8_t>(std::clamp((x + y) * 180 / (width + height) + grain, 0, 255));
        }
        f.pixels[i] = b; f.pixels[i + 1] = g; f.pixels[i + 2] = r;
        f.pixels[i + 3] = kind == 6 ? static_cast<uint8_t>((x + y) % 256) : 255;
    }
    return f;
}
double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end()); return values[values.size() / 2];
}
int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 2 || argc == 3);
        fs::create_directories(argv[1]);
        std::vector<Frame> frames;
        for (int width : {160, 320, 640}) for (int kind = 0; kind < 7; ++kind)
            frames.push_back(Fixture(width, width * 9 / 16, kind));
        if (argc == 3) {
            size_t n = 0;
            for (const auto& entry : fs::recursive_directory_iterator(argv[2])) {
                if (!entry.is_regular_file()) continue;
                std::ifstream input(entry.path(), std::ios::binary);
                std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
                ThumbnailDiskHeader header; std::vector<uint8_t> pixels;
                if (ThumbnailCodec::Decode(bytes, header, pixels))
                    frames.push_back({"decoded-cache-" + std::to_string(++n), int(header.width), int(header.height), std::move(pixels)});
            }
        }
        std::ofstream csv(fs::path(argv[1]) / "codec.csv");
        csv << "frame,width,height,fixed_bytes,adaptive_bytes,format,fixed_encode_ms,adaptive_encode_ms,identical_pixels\n";
        uint64_t before = 0, after = 0;
        for (const auto& f : frames) {
            auto fixed = FixedJpeg(f); std::string adaptive;
            Require(ThumbnailCodec::Encode(f.pixels, f.width, f.height, 12.5, adaptive));
            Require(adaptive.size() <= fixed.size());
            ThumbnailDiskHeader header; std::vector<uint8_t> decoded;
            Require(ThumbnailCodec::Decode(adaptive, header, decoded));
            Require(header.width == f.width && header.height == f.height && header.timestamp == 12.5);
            if (header.version == 2) Require(decoded == f.pixels);
            else Require(adaptive == fixed);
            std::vector<double> fixedTimes, adaptiveTimes;
            auto timeFixed = [&] {
                const auto start = std::chrono::steady_clock::now(); fixed = FixedJpeg(f);
                fixedTimes.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            };
            auto timeAdaptive = [&] {
                const auto start = std::chrono::steady_clock::now();
                Require(ThumbnailCodec::Encode(f.pixels, f.width, f.height, 12.5, adaptive));
                adaptiveTimes.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            };
            for (int round = 0; round < 20; ++round) {
                if (round % 2) { timeAdaptive(); timeFixed(); } else { timeFixed(); timeAdaptive(); }
            }
            before += fixed.size(); after += adaptive.size();
            csv << f.name << ',' << f.width << ',' << f.height << ',' << fixed.size() << ',' << adaptive.size() << ','
                << (header.version == 2 ? "PNG" : "JPEG") << ',' << Median(fixedTimes) << ',' << Median(adaptiveTimes)
                << ',' << (decoded == f.pixels ? "true" : "false") << '\n';
        }
        Require(csv.good());
        std::cout << frames.size() << " codec frames: " << before << " -> " << after << " bytes\n";
        return 0;
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
