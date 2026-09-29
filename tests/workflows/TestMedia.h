#pragma once
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <cstdint>

// Self-contained, silent PCM media. No downloads, codecs, or personal files.
inline void WriteWorkflowWave(const std::filesystem::path& path, unsigned seconds) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    const uint32_t bytes = seconds * 8000u * 2u;
    auto u16 = [&](uint16_t value) { out.put(char(value)); out.put(char(value >> 8)); };
    auto u32 = [&](uint32_t value) { u16(uint16_t(value)); u16(uint16_t(value >> 16)); };
    out.write("RIFF", 4); u32(36 + bytes); out.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(1); u32(8000); u32(16000); u16(2); u16(16);
    out.write("data", 4); u32(bytes);
    char silence[4096]{};
    for (uint32_t left = bytes; left;) {
        const uint32_t count = left < sizeof(silence) ? left : uint32_t(sizeof(silence));
        out.write(silence, count); left -= count;
    }
    if (!out) throw std::runtime_error("Could not generate WAV fixture");
}
