#include "ChannelPolicy.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <regex>

namespace Aperture {

namespace {

std::string ToLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return str;
}

std::vector<std::string> SplitDots(const std::string& str) {
    std::vector<std::string> parts;
    std::istringstream ss(str);
    std::string token;
    while (std::getline(ss, token, '.')) { parts.push_back(token); }
    return parts;
}

bool IsAllDigits(const std::string& str) {
    if (str.empty()) return false;
    for (char c : str) { if (!std::isdigit(static_cast<unsigned char>(c))) return false; }
    return true;
}

} // namespace

namespace SemVerUtils {

bool IsValidPrereleaseIdentifier(const std::string& ident) {
    if (ident.empty()) return false;
    for (char c : ident) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') { return false; }
    }
    // Numeric identifiers MUST NOT include leading zeroes
    if (IsAllDigits(ident) && ident.length() > 1 && ident[0] == '0') { return false; }
    return true;
}

bool IsValidPrereleaseString(const std::string& prerelease) {
    if (prerelease.empty()) return false;
    auto parts = SplitDots(prerelease);
    if (parts.empty()) return false;
    for (const auto& p : parts) { if (!IsValidPrereleaseIdentifier(p)) return false; }
    return true;
}

int ComparePrerelease(const std::string& a, const std::string& b) {
    if (a == b) return 0;
    if (a.empty() && !b.empty()) return 1;  // Normal version > Pre-release
    if (!a.empty() && b.empty()) return -1; // Pre-release < Normal version
    auto partsA = SplitDots(a);
    auto partsB = SplitDots(b);
    size_t minLen = (std::min)(partsA.size(), partsB.size());
    for (size_t i = 0; i < minLen; ++i) {
        const std::string& idA = partsA[i];
        const std::string& idB = partsB[i];
        if (idA == idB) continue;
        bool isNumA = IsAllDigits(idA);
        bool isNumB = IsAllDigits(idB);
        if (isNumA && isNumB) {
            try {
                unsigned long long nA = std::stoull(idA);
                unsigned long long nB = std::stoull(idB);
                if (nA != nB) { return nA < nB ? -1 : 1; }
            } catch (...) {
                if (idA.length() != idB.length()) { return idA.length() < idB.length() ? -1 : 1; }
                return idA < idB ? -1 : 1;
            }
        } else if (isNumA && !isNumB) {
            // Numeric identifiers always have lower precedence than non-numeric identifiers
            return -1;
        } else if (!isNumA && isNumB) { return 1; } else {
            // Lexical comparison in ASCII sort order
            return idA < idB ? -1 : 1;
        }
    }
    // A larger set of pre-release fields has a higher precedence than a smaller set
    if (partsA.size() != partsB.size()) { return partsA.size() < partsB.size() ? -1 : 1; }
    return 0;
}

} // namespace SemVerUtils

ChannelPolicy ChannelPolicy::CreateDefault() {
    ChannelPolicy policy;
    policy.schemaVersion_ = 1;
    policy.defaultChannel_ = "stable";
    ChannelDefinition stableChan;
    stableChan.id = "stable";
    stableChan.displayName = L"Stable";
    stableChan.description = "Official stable releases tested for general daily use.";
    stableChan.stabilityRank = 0;
    stableChan.acceptedChannels = { "stable" };
    stableChan.prerelease.allowed = false;
    stableChan.prerelease.prefix = "";
    stableChan.prerelease.pattern = "^$";
    stableChan.prerelease.requireNumericSequence = false;
    stableChan.retentionDays = 180;
    stableChan.maxExpiryDays = 90;
    stableChan.updateCadenceDays = 30;
    stableChan.allowDowngrade = false;
    stableChan.requiresPinnedTrust = true;
    ChannelDefinition betaChan;
    betaChan.id = "beta";
    betaChan.displayName = L"Beta";
    betaChan.description = "Preview releases for testing upcoming features before stable rollout.";
    betaChan.stabilityRank = 1;
    betaChan.acceptedChannels = { "beta", "stable" };
    betaChan.prerelease.allowed = true;
    betaChan.prerelease.prefix = "beta";
    betaChan.prerelease.pattern = "^beta(\\.[0-9]+)?$";
    betaChan.prerelease.requireNumericSequence = true;
    betaChan.retentionDays = 60;
    betaChan.maxExpiryDays = 30;
    betaChan.updateCadenceDays = 14;
    betaChan.allowDowngrade = false;
    betaChan.requiresPinnedTrust = true;
    ChannelDefinition nightlyChan;
    nightlyChan.id = "nightly";
    nightlyChan.displayName = L"Nightly";
    nightlyChan.description = "Bleeding-edge automated daily builds containing latest development changes.";
    nightlyChan.stabilityRank = 2;
    nightlyChan.acceptedChannels = { "nightly", "beta", "stable" };
    nightlyChan.prerelease.allowed = true;
    nightlyChan.prerelease.prefix = "nightly";
    nightlyChan.prerelease.pattern = "^nightly(\\.[0-9]{8}(\\.[0-9]+)?)?$";
    nightlyChan.prerelease.requireNumericSequence = true;
    nightlyChan.retentionDays = 14;
    nightlyChan.maxExpiryDays = 14;
    nightlyChan.updateCadenceDays = 1;
    nightlyChan.allowDowngrade = false;
    nightlyChan.requiresPinnedTrust = true;
    policy.channels_ = { stableChan, betaChan, nightlyChan };
    policy.RebuildIndex();
    return policy;
}

const ChannelPolicy& ChannelPolicy::Instance() { static ChannelPolicy instance = CreateDefault(); return instance; }

void ChannelPolicy::RebuildIndex() {
    channelIndexMap_.clear();
    for (size_t i = 0; i < channels_.size(); ++i) { channelIndexMap_[ToLower(channels_[i].id)] = i; }
}

const std::string& ChannelPolicy::GetDefaultChannel() const { return defaultChannel_; }

const std::vector<ChannelDefinition>& ChannelPolicy::GetChannels() const { return channels_; }

const ChannelDefinition* ChannelPolicy::FindChannel(const std::string& channelId) const {
    std::string lower = ToLower(channelId);
    auto it = channelIndexMap_.find(lower);
    if (it != channelIndexMap_.end() && it->second < channels_.size()) { return &channels_[it->second]; }
    return nullptr;
}

bool ChannelPolicy::IsValidChannel(const std::string& channelId) const { return FindChannel(channelId) != nullptr; }

std::string ChannelPolicy::CanonicalizeChannel(const std::string& channelId) const {
    const ChannelDefinition* chan = FindChannel(channelId);
    if (chan) { return chan->id; }
    return defaultChannel_;
}

bool ChannelPolicy::IsChannelAccepted(const std::string& currentChannel, const std::string& candidateChannel) const {
    const ChannelDefinition* cur = FindChannel(currentChannel);
    if (!cur) {
        // Unknown channels fail closed
        return false;
    }
    std::string candLower = ToLower(candidateChannel);
    for (const auto& acc : cur->acceptedChannels) {
        if (ToLower(acc) == candLower) { return true; }
    }
    return false;
}

bool ChannelPolicy::IsValidPrereleaseForChannel(const std::string& channelId, const std::string& prerelease) const {
    const ChannelDefinition* chan = FindChannel(channelId);
    if (!chan) { return false; }
    if (!chan->prerelease.allowed) { return prerelease.empty(); }
    if (prerelease.empty()) {
        // A channel that allows prerelease may or may not require one depending on context,
        // but for prerelease tag validation, empty is false here.
        return false;
    }
    if (!SemVerUtils::IsValidPrereleaseString(prerelease)) { return false; }
    auto parts = SplitDots(prerelease);
    if (parts.empty()) return false;
    if (!chan->prerelease.prefix.empty()) {
        if (parts[0] != chan->prerelease.prefix) { return false; }
    }
    if (chan->prerelease.requireNumericSequence && parts.size() > 1) {
        for (size_t i = 1; i < parts.size(); ++i) {
            if (!IsAllDigits(parts[i])) { return false; }
        }
    }
    return true;
}

bool ChannelPolicy::IsStabilityDowngrade(const std::string& fromChannel, const std::string& toChannel) const {
    const ChannelDefinition* from = FindChannel(fromChannel);
    const ChannelDefinition* to = FindChannel(toChannel);
    if (!from || !to) { return false; }
    // Stability rank 0 is most stable (stable). Higher rank means less stable (nightly).
    // Moving from rank 0 to rank 1 or 2 is a move to a less stable channel.
    return from->stabilityRank < to->stabilityRank;
}

std::wstring ChannelPolicy::GetChannelDisplayName(const std::string& channelId) const {
    const ChannelDefinition* chan = FindChannel(channelId);
    if (chan) { return chan->displayName; }
    return L"Stable";
}

std::string ChannelPolicy::GetChannelDescription(const std::string& channelId) const {
    const ChannelDefinition* chan = FindChannel(channelId);
    if (chan) { return chan->description; }
    return "";
}

int ChannelPolicy::GetChannelMaxExpiryDays(const std::string& channelId) const {
    const ChannelDefinition* chan = FindChannel(channelId);
    if (chan) { return chan->maxExpiryDays; }
    return 90;
}

bool ChannelPolicy::LoadFromJson(const std::string& jsonText, std::string& outError) {
    outError.clear();
    JsonValue root;
    if (!JsonValue::parse(jsonText, root) || !root.is_object()) { outError = "Malformed ChannelPolicy JSON"; return false; }
    int schemaVer = root.get("schemaVersion").as_int(1);
    std::string defChan = root.get("defaultChannel").as_string("stable");
    if (!root.has_key("channels") || !root.get("channels").is_array()) {
        outError = "ChannelPolicy JSON missing 'channels' array";
        return false;
    }
    const auto& arr = root.get("channels").as_array();
    if (arr.empty()) { outError = "ChannelPolicy 'channels' array cannot be empty"; return false; }
    std::vector<ChannelDefinition> parsedChannels;
    for (const auto& item : arr) {
        if (!item.is_object()) continue;
        ChannelDefinition def;
        def.id = item.get("id").as_string("");
        if (def.id.empty()) { outError = "Channel definition missing 'id'"; return false; }
        std::string dName = item.get("displayName").as_string(def.id);
        def.displayName = Utf8ToUtf16(dName);
        def.description = item.get("description").as_string("");
        def.stabilityRank = item.get("stabilityRank").as_int(0);
        if (item.has_key("acceptedChannels") && item.get("acceptedChannels").is_array()) {
            for (const auto& a : item.get("acceptedChannels").as_array()) {
                if (a.is_string()) { def.acceptedChannels.push_back(a.as_string()); }
            }
        }
        if (def.acceptedChannels.empty()) { def.acceptedChannels.push_back(def.id); }
        if (item.has_key("prerelease") && item.get("prerelease").is_object()) {
            const auto& pre = item.get("prerelease");
            def.prerelease.allowed = pre.get("allowed").as_bool(false);
            def.prerelease.prefix = pre.get("prefix").as_string("");
            def.prerelease.pattern = pre.get("pattern").as_string("");
            def.prerelease.requireNumericSequence = pre.get("requireNumericSequence").as_bool(false);
        }
        def.retentionDays = item.get("retentionDays").as_int(180);
        def.maxExpiryDays = item.get("maxExpiryDays").as_int(90);
        def.updateCadenceDays = item.get("updateCadenceDays").as_int(30);
        def.allowDowngrade = item.get("allowDowngrade").as_bool(false);
        def.requiresPinnedTrust = item.get("requiresPinnedTrust").as_bool(true);
        parsedChannels.push_back(def);
    }
    schemaVersion_ = schemaVer;
    defaultChannel_ = defChan;
    channels_ = std::move(parsedChannels);
    RebuildIndex();
    return true;
}

} // namespace Aperture
