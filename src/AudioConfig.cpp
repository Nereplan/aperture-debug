#include "AudioConfig.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
namespace Aperture {

namespace {

std::string Trim(const std::string& str) {
    size_t start = 0;
    while (start < str.size() && (static_cast<unsigned char>(str[start]) <= ' ' || str[start] == '\t' || str[start] == '\r' || str[start] == '\n')) {
        start++;
    }
    size_t end = str.size();
    while (end > start && (static_cast<unsigned char>(str[end - 1]) <= ' ' || str[end - 1] == '\t' || str[end - 1] == '\r' || str[end - 1] == '\n')) {
        end--;
    }
    return str.substr(start, end - start);
}

std::string ToLower(const std::string& str) {
    std::string out = str;
    for (char& c : out) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return out;
}

} // anonymous namespace

std::string AudioConfig::NormalizeAudioDevice( const std::string& rawDevice, const std::vector<AudioDeviceInfo>& availableDevices,
    bool* outFellBack ) {
    if (outFellBack) *outFellBack = false;
    std::string trimmed = Trim(rawDevice);
    if (trimmed.empty()) { return "auto"; }
    std::string lower = ToLower(trimmed);
    if (lower == "auto" || lower == "default" || lower == "autoselect") { return "auto"; }
    // If available devices list is populated, check if the device still exists
    if (!availableDevices.empty()) {
        bool found = false;
        for (const auto& dev : availableDevices) {
            if (dev.name == trimmed) { found = true; break; }
        }
        if (!found) {
            // Requested device is not in the enumerated list -> fall back to "auto"
            if (outFellBack) *outFellBack = true;
            return "auto";
        }
    }
    return trimmed;
}

std::string AudioConfig::NormalizeAudioLayout(const std::string& rawLayout) {
    std::string lower = ToLower(Trim(rawLayout));
    if (lower == "auto") return "auto";
    if (lower == "auto-safe" || lower == "autosafe" || lower == "safe") return "auto-safe";
    if (lower == "stereo" || lower == "2.0" || lower == "2ch" || lower == "2") return "stereo";
    if (lower == "5.1" || lower == "5.1(side)" || lower == "surround51" || lower == "6ch") return "5.1";
    if (lower == "7.1" || lower == "surround71" || lower == "8ch") return "7.1";
    return "auto-safe";
}

std::string AudioConfig::NormalizeAudioChannelMix(const std::string& rawMix) {
    std::string lower = ToLower(Trim(rawMix));
    if (lower == "auto") return "auto";
    if (lower == "none" || lower == "off" || lower == "direct" || lower == "passthrough") return "none";
    if (lower == "downmix-stereo" || lower == "downmix" || lower == "stereo-downmix") return "downmix-stereo";
    if (lower == "upmix-surround" || lower == "upmix" || lower == "surround-upmix") return "upmix-surround";
    return "auto";
}

std::string AudioConfig::NormalizeNightMode(const std::string& rawNightMode) {
    std::string lower = ToLower(Trim(rawNightMode));
    if (lower == "off" || lower == "none" || lower == "disabled" || lower == "false" || lower == "0") return "off";
    if (lower == "light" || lower == "low") return "light";
    if (lower == "medium" || lower == "med" || lower == "standard" || lower == "mid") return "medium";
    if (lower == "strong" || lower == "high" || lower == "max") return "strong";
    return "off";
}

double AudioConfig::NormalizeAudioBalance(double rawBalance) {
    if (!std::isfinite(rawBalance)) { return 0.0; }
    return std::clamp(rawBalance, -1.0, 1.0);
}

std::string AudioConfig::NormalizeAudioChannelMap(const std::string& rawChannelMap) {
    std::string lower = ToLower(Trim(rawChannelMap));
    if (lower.empty() || lower == "auto" || lower == "default" || lower == "none" || lower == "off") { return "auto"; }
    if (lower == "stereo" || lower == "standard" || lower == "normal" || lower == "2ch" || lower == "2.0") { return "stereo"; }
    if (lower == "mono-left" || lower == "mono_left" || lower == "monoleft" || lower == "left" || lower == "left-only" || lower == "leftonly")
        return "mono-left";
    if (lower == "mono-right" || lower == "mono_right" || lower == "monoright" || lower == "right" || lower == "right-only" || lower == "rightonly")
        return "mono-right";
    if (lower == "swap" || lower == "swapped" || lower == "reverse" || lower == "reversed" || lower == "flip" || lower == "inverted" || lower == "lr-swap" || lower == "rl")
        return "swap";
    return "auto";
}

std::string AudioConfig::NormalizeReplayGainMode(const std::string& rawMode) {
    std::string lower = ToLower(Trim(rawMode));
    if (lower == "track" || lower == "album") { return lower; }
    return "off";
}

double AudioConfig::NormalizeReplayGainPreampDb(double rawDb) {
    if (!std::isfinite(rawDb)) { return 0.0; }
    return std::clamp(rawDb, -15.0, 15.0);
}

std::string AudioConfig::GetNightModeFilter(const std::string& normalizedNightMode) {
    if (normalizedNightMode == "light") { return "lavfi=[dynaudnorm=f=250:g=15:p=0.90:m=5.0]"; } else if (normalizedNightMode == "medium") {
        return "lavfi=[dynaudnorm=f=200:g=21:p=0.95:m=10.0]";
    } else if (normalizedNightMode == "strong") { return "lavfi=[dynaudnorm=f=150:g=27:p=0.95:m=15.0]"; }
    return "";
}

std::string AudioConfig::GetChannelMapFilter(const std::string& normalizedChannelMap) {
    if (normalizedChannelMap == "mono-left") { return "lavfi=[pan=stereo|c0=c0|c1=c0]"; } else if (normalizedChannelMap == "mono-right") {
        return "lavfi=[pan=stereo|c0=c1|c1=c1]";
    } else if (normalizedChannelMap == "swap") { return "lavfi=[pan=stereo|c0=c1|c1=c0]"; }
    return "";
}

AudioManagementPlan AudioConfig::BuildAudioPlan( const std::string& device, const std::string& layout, const std::string& channelMix,
    bool normalizeDownmix, const std::string& nightMode, double balance, const std::string& channelMap,
    const std::vector<AudioDeviceInfo>& availableDevices, const std::string& replayGainMode, double replayGainPreampDb,
    bool audioPassthrough, bool pitchCorrection ) {
    AudioManagementPlan plan;
    plan.requestedDevice = device;
    plan.requestedLayout = layout;
    plan.requestedChannelMix = channelMix;
    plan.requestedNormalizeDownmix = normalizeDownmix;
    plan.requestedNightMode = nightMode;
    plan.requestedBalance = balance;
    plan.requestedChannelMap = channelMap;
    plan.requestedReplayGainMode = replayGainMode;
    plan.requestedReplayGainPreampDb = replayGainPreampDb;
    plan.requestedAudioPassthrough = audioPassthrough;
    plan.requestedPitchCorrection = pitchCorrection;
    bool fellBack = false;
    plan.normalizedDevice = NormalizeAudioDevice(device, availableDevices, &fellBack);
    plan.deviceFellBackToAuto = fellBack;
    plan.normalizedLayout = NormalizeAudioLayout(layout);
    plan.normalizedChannelMix = NormalizeAudioChannelMix(channelMix);
    plan.normalizedNormalizeDownmix = normalizeDownmix;
    plan.normalizedNightMode = NormalizeNightMode(nightMode);
    plan.normalizedBalance = NormalizeAudioBalance(balance);
    plan.normalizedChannelMap = NormalizeAudioChannelMap(channelMap);
    plan.normalizedReplayGainMode = NormalizeReplayGainMode(replayGainMode);
    plan.normalizedReplayGainPreampDb = NormalizeReplayGainPreampDb(replayGainPreampDb);
    plan.normalizedAudioPassthrough = audioPassthrough;
    plan.normalizedPitchCorrection = pitchCorrection;
    std::string nightFilter = GetNightModeFilter(plan.normalizedNightMode);
    std::string channelFilter = GetChannelMapFilter(plan.normalizedChannelMap);
    if (!nightFilter.empty() && !channelFilter.empty()) {
        plan.filterString = nightFilter + "," + channelFilter;
    } else if (!nightFilter.empty()) { plan.filterString = nightFilter; } else if (!channelFilter.empty()) {
        plan.filterString = channelFilter;
    } else { plan.filterString = ""; }
    // 1. Audio Device Action
    plan.actions.push_back({ "audio-device", plan.normalizedDevice });
    // 2. Audio Channels / Layout Action
    std::string effectiveChannels = plan.normalizedLayout;
    if (plan.normalizedChannelMix == "downmix-stereo") {
        effectiveChannels = "stereo";
    } else if (plan.normalizedChannelMix == "upmix-surround") {
        if (plan.normalizedLayout == "stereo" || plan.normalizedLayout == "auto" || plan.normalizedLayout == "auto-safe")
            effectiveChannels = "5.1";
    }
    plan.actions.push_back({ "audio-channels", effectiveChannels });
    // 3. Downmix Normalization Action
    plan.actions.push_back({ "audio-normalize-downmix", plan.normalizedNormalizeDownmix ? "yes" : "no" });
    // 4. Audio Pitch Correction
    plan.actions.push_back({ "audio-pitch-correction", plan.normalizedPitchCorrection ? "yes" : "no" });
    // 5. Audio Balance Action
    double normBal = plan.normalizedBalance;
    if (std::abs(normBal) < 0.0001) normBal = 0.0;
    char balanceBuf[32];
    snprintf(balanceBuf, sizeof(balanceBuf), "%.2f", normBal);
    plan.actions.push_back({ "balance", balanceBuf });
    // 6. Audio Filters Action (DRC / Channel Mapping)
    plan.actions.push_back({ "af", plan.filterString });
    // 7. ReplayGain Mode Action ("off" maps to mpv's "no")
    plan.actions.push_back({ "replaygain", plan.normalizedReplayGainMode == "off" ? "no" : plan.normalizedReplayGainMode });
    // 8. ReplayGain Preamp Action (dB)
    double normPreamp = plan.normalizedReplayGainPreampDb;
    if (std::abs(normPreamp) < 0.0001) normPreamp = 0.0;
    char preampBuf[32];
    snprintf(preampBuf, sizeof(preampBuf), "%.2f", normPreamp);
    plan.actions.push_back({ "replaygain-preamp", preampBuf });
    // 9. S/PDIF / HDMI Bitstream Passthrough Action
    plan.actions.push_back({ "audio-spdif", plan.normalizedAudioPassthrough ? "ac3,dts,eac3,dtshd,truehd" : "no" });
    return plan;
}

} // namespace Aperture
