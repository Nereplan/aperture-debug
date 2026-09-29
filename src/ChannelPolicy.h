#pragma once

#include "Common.h"
#include "Json.h"
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace Aperture {

struct ChannelPrereleaseRule { bool allowed = false; std::string prefix; std::string pattern; bool requireNumericSequence = false; };

struct ChannelDefinition {
    std::string id;
    std::wstring displayName;
    std::string description;
    int stabilityRank = 0;
    std::vector<std::string> acceptedChannels;
    ChannelPrereleaseRule prerelease;
    int retentionDays = 180, maxExpiryDays = 90, updateCadenceDays = 30;
    bool allowDowngrade = false, requiresPinnedTrust = true;
};

class ChannelPolicy {
public:
    static const ChannelPolicy& Instance();
    const std::string& GetDefaultChannel() const;
    const std::vector<ChannelDefinition>& GetChannels() const;
    const ChannelDefinition* FindChannel(const std::string& channelId) const;
    bool IsValidChannel(const std::string& channelId) const;
    std::string CanonicalizeChannel(const std::string& channelId) const;
    // Checks if candidateChannel is accepted for an update by configured currentChannel
    bool IsChannelAccepted(const std::string& currentChannel, const std::string& candidateChannel) const;
    // Validates whether a SemVer prerelease string conforms to the channel's prerelease rules
    bool IsValidPrereleaseForChannel(const std::string& channelId, const std::string& prerelease) const;
    // Returns true if moving from fromChannel to toChannel is a move to a less stable channel (lower stability rank)
    bool IsStabilityDowngrade(const std::string& fromChannel, const std::string& toChannel) const;
    std::wstring GetChannelDisplayName(const std::string& channelId) const;
    std::string GetChannelDescription(const std::string& channelId) const;
    int GetChannelMaxExpiryDays(const std::string& channelId) const;
    // Load / parse policy from JSON text (for testing and runtime overrides)
    bool LoadFromJson(const std::string& jsonText, std::string& outError);
    // Creates an instance with default built-in policy (stable, beta, nightly)
    static ChannelPolicy CreateDefault();

private:
    int schemaVersion_ = 1;
    std::string defaultChannel_ = "stable";
    std::vector<ChannelDefinition> channels_;
    std::map<std::string, size_t> channelIndexMap_;
    void RebuildIndex();
};

// SemVer 2.0.0 Prerelease Helper Functions
namespace SemVerUtils {
    bool IsValidPrereleaseIdentifier(const std::string& ident);
    bool IsValidPrereleaseString(const std::string& prerelease);
    int ComparePrerelease(const std::string& a, const std::string& b);
}

} // namespace Aperture
