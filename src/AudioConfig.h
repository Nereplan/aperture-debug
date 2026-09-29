#pragma once

#include "Common.h"
#include <string>
#include <vector>

namespace Aperture {

struct AudioDeviceInfo {
    std::string name;             // mpv device name, e.g. "auto", "wasapi/{0.0.0.00000000}.{...}"
    std::wstring description;     // friendly name, e.g. L"Autoselect device", L"Speakers (Realtek Audio)"
    bool isDefault = false;
};

struct AudioPropertyAction { std::string property; std::string value; };

struct AudioPropertyResult {
    std::string property, value;
    bool success = false;
    int mpvErrorCode = 0;
    std::string message;
};

struct AudioManagementPlan {
    std::string requestedDevice, requestedLayout, requestedChannelMix;
    bool requestedNormalizeDownmix = true;
    std::string requestedNightMode;
    double requestedBalance = 0.0;
    std::string requestedChannelMap = "auto";
    std::string normalizedDevice, normalizedLayout, normalizedChannelMix;
    bool normalizedNormalizeDownmix = true;
    std::string normalizedNightMode;
    double normalizedBalance = 0.0;
    std::string normalizedChannelMap = "auto";
    bool deviceFellBackToAuto = false;
    std::string filterString;
    std::vector<AudioPropertyAction> actions;
};

class AudioConfig {
public:
    static std::string NormalizeAudioDevice( const std::string& rawDevice, const std::vector<AudioDeviceInfo>& availableDevices = {},
        bool* outFellBack = nullptr );
    static std::string NormalizeAudioLayout(const std::string& rawLayout);
    static std::string NormalizeAudioChannelMix(const std::string& rawMix);
    static std::string NormalizeNightMode(const std::string& rawNightMode);
    static double NormalizeAudioBalance(double rawBalance);
    static std::string NormalizeAudioChannelMap(const std::string& rawChannelMap);
    static std::string GetNightModeFilter(const std::string& normalizedNightMode);
    static std::string GetChannelMapFilter(const std::string& normalizedChannelMap);
    static AudioManagementPlan BuildAudioPlan( const std::string& device, const std::string& layout, const std::string& channelMix,
        bool normalizeDownmix, const std::string& nightMode, double balance, const std::string& channelMap,
        const std::vector<AudioDeviceInfo>& availableDevices = {} );
    static AudioManagementPlan BuildAudioPlan( const std::string& device, const std::string& layout, const std::string& channelMix,
        bool normalizeDownmix, const std::string& nightMode, const std::vector<AudioDeviceInfo>& availableDevices = {}
    ) { return BuildAudioPlan(device, layout, channelMix, normalizeDownmix, nightMode, 0.0, "auto", availableDevices); }
};

} // namespace Aperture
