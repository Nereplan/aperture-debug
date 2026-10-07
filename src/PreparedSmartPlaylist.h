#pragma once

#include "MediaLibrary.h"
#include <cwctype>
#include <limits>

namespace Aperture {

// One immutable evaluation program per scan. Validation, rule constants and
// relative cutoffs are resolved once; every item sees the same wall-clock time.
class PreparedSmartPlaylist {
public:
    struct Possibilities { bool present = true, missing = true; };

    explicit PreparedSmartPlaylist(const SmartPlaylistCriteria& criteria, int64_t nowSec)
        : matchAll(criteria.matchMode == SmartMatchMode::MatchAll), sortField(criteria.sortField),
          sortDirection(criteria.sortDirection), limit(criteria.limit), evaluatedAtSec(nowSec) {
        valid = criteria.Validate();
        if (!valid) return;
        playbackDependent = sortField == 2 || sortField == 4; // LastPlayed / Progress
        rules.reserve(criteria.rules.size());
        for (const auto& source : criteria.rules) {
            Rule rule;
            rule.field = source.field; rule.op = source.op;
            rule.number = source.numericValue; rule.relative = source.relativeDate;
            if (source.field == SmartRuleField::Title || source.field == SmartRuleField::Path) {
                rule.pattern = source.GetLowerPattern();
            } else if (source.field == SmartRuleField::Status) {
                rule.status = NormalizeStatus(source.value);
                rule.missing = rule.status == "missing";
                requiresAvailability |= rule.missing;
                const bool availabilityOperator = rule.op == SmartRuleOperator::Equals || rule.op == SmartRuleOperator::Contains ||
                    rule.op == SmartRuleOperator::NotEquals || rule.op == SmartRuleOperator::DoesNotContain;
                playbackDependent |= !rule.missing || !availabilityOperator;
            }
            playbackDependent |= source.field == SmartRuleField::Progress || source.field == SmartRuleField::LastWatched;
            if ((source.field == SmartRuleField::DateAdded || source.field == SmartRuleField::LastWatched) && rule.relative) {
                relativeRules.push_back(rules.size());
                rule.cutoff = Subtract(nowSec, static_cast<int64_t>(rule.number));
            } else if (source.field == SmartRuleField::DateAdded || source.field == SmartRuleField::LastWatched) {
                rule.cutoff = static_cast<int64_t>(rule.number);
            }
            rules.push_back(std::move(rule));
        }
    }

    bool IsValid() const { return valid; }
    bool RequiresAvailability() const { return requiresAvailability; }
    bool DependsOnPlayback() const { return playbackDependent; }
    bool DependsOnTime() const { return !relativeRules.empty(); }
    int SortField() const { return sortField; }
    int SortDirection() const { return sortDirection; }
    size_t Limit() const { return limit; }
    int64_t EvaluatedAtSec() const { return evaluatedAtSec; }

    Possibilities MatchWithoutDisk(const MediaItem& item) const {
        if (!valid) return {false, false};
        if (rules.empty()) return {true, true};
        Possibilities result{matchAll, matchAll};
        std::wstring title, path;
        bool titleReady = false, pathReady = false;
        for (const auto& rule : rules) {
            Possibilities matched;
            if (rule.field == SmartRuleField::Title || rule.field == SmartRuleField::Path) {
                auto& target = rule.field == SmartRuleField::Title ? title : path;
                auto& ready = rule.field == SmartRuleField::Title ? titleReady : pathReady;
                if (!ready) {
                    target = rule.field == SmartRuleField::Title ? std::wstring(item.GetDisplayTitleView()) : item.filePath;
                    for (auto& c : target) c = static_cast<wchar_t>(std::towlower(c));
                    ready = true;
                }
                matched.present = matched.missing = StringMatches(target, rule.op, rule.pattern);
            } else if (rule.field == SmartRuleField::Status) {
                matched.present = StatusMatches(item.status, rule, true);
                matched.missing = StatusMatches(item.status, rule, false);
            } else {
                bool value = false;
                switch (rule.field) {
                case SmartRuleField::Duration: value = NumericMatches(item.durationSec, rule.op, static_cast<double>(rule.number)); break;
                case SmartRuleField::Progress: value = NumericMatches(item.GetProgressRatio(), rule.op, static_cast<double>(rule.number)); break;
                case SmartRuleField::FileSize: value = NumericMatches(item.fileSizeBytes, rule.op, static_cast<uint64_t>(rule.number)); break;
                case SmartRuleField::DateAdded: value = NumericMatches(item.addedTimestamp, rule.op, rule.cutoff); break;
                case SmartRuleField::LastWatched: value = NumericMatches(item.lastWatchedTimestamp, rule.op, rule.cutoff); break;
                case SmartRuleField::VideoHeight: value = NumericMatches(item.videoHeight, rule.op, static_cast<uint32_t>(rule.number)); break;
                default: break;
                }
                matched.present = matched.missing = value;
            }
            if (matchAll) {
                result.present &= matched.present; result.missing &= matched.missing;
                if (!result.present && !result.missing) break;
            } else {
                result.present |= matched.present; result.missing |= matched.missing;
                if (result.present && result.missing) break;
            }
        }
        return result;
    }

    // At most two integer-second boundaries can change a relative comparison:
    // equality with its cutoff, and the following second. Tracking all rules
    // is conservative even when another condition currently rejects the item.
    int64_t NextChangeAfter(const MediaItem& item) const {
        int64_t next = (std::numeric_limits<int64_t>::max)();
        for (size_t index : relativeRules) {
            const auto& rule = rules[index];
            const int64_t timestamp = rule.field == SmartRuleField::DateAdded ? item.addedTimestamp : item.lastWatchedTimestamp;
            const int64_t boundary = Add(timestamp, static_cast<int64_t>(rule.number));
            if (boundary > evaluatedAtSec) next = (std::min)(next, boundary);
            const int64_t after = Add(boundary, 1);
            if (after > evaluatedAtSec) next = (std::min)(next, after);
        }
        return next;
    }

private:
    struct Rule {
        SmartRuleField field = SmartRuleField::Title;
        SmartRuleOperator op = SmartRuleOperator::Contains;
        std::wstring pattern;
        std::string status;
        long double number = 0;
        int64_t cutoff = 0;
        bool relative = false, missing = false;
    };
    static int64_t Add(int64_t a, int64_t b) {
        const auto maximum = (std::numeric_limits<int64_t>::max)();
        return a > maximum - b ? maximum : a + b; // b is nonnegative
    }
    static int64_t Subtract(int64_t a, int64_t b) {
        const auto minimum = (std::numeric_limits<int64_t>::min)();
        return a < minimum + b ? minimum : a - b;
    }
    static std::string NormalizeStatus(const std::string& value) {
        const auto first = value.find_first_not_of(" \t\r\n\"");
        if (first == std::string::npos) return {};
        const auto last = value.find_last_not_of(" \t\r\n\"");
        std::string result = value.substr(first, last - first + 1);
        for (auto& c : result) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (result == "unwatched") return "new";
        if (result == "inprogress" || result == "watching") return "in_progress";
        if (result == "watched" || result == "completed") return "done";
        return result;
    }
    static bool StatusMatches(const std::string& status, const Rule& rule, bool present) {
        const bool equals = rule.op == SmartRuleOperator::Equals || rule.op == SmartRuleOperator::Contains;
        const bool notEquals = rule.op == SmartRuleOperator::NotEquals || rule.op == SmartRuleOperator::DoesNotContain;
        if (rule.missing && (equals || notEquals)) return equals ? !present : present;
        return notEquals ? status != rule.status : status == rule.status;
    }
    static bool StringMatches(const std::wstring& target, SmartRuleOperator op, const std::wstring& pattern) {
        if (op == SmartRuleOperator::DoesNotContain) return target.find(pattern) == std::wstring::npos;
        if (op == SmartRuleOperator::Equals) return target == pattern;
        if (op == SmartRuleOperator::NotEquals) return target != pattern;
        if (op == SmartRuleOperator::StartsWith) return target.rfind(pattern, 0) == 0;
        if (op == SmartRuleOperator::EndsWith)
            return target.size() >= pattern.size() && target.compare(target.size() - pattern.size(), pattern.size(), pattern) == 0;
        return target.find(pattern) != std::wstring::npos;
    }
    template<class T> static bool NumericMatches(T value, SmartRuleOperator op, T expected) {
        if (op == SmartRuleOperator::NotEquals) return value != expected;
        if (op == SmartRuleOperator::GreaterThan) return value > expected;
        if (op == SmartRuleOperator::LessThan) return value < expected;
        if (op == SmartRuleOperator::GreaterThanOrEqual) return value >= expected;
        if (op == SmartRuleOperator::LessThanOrEqual) return value <= expected;
        return value == expected;
    }
    std::vector<Rule> rules;
    std::vector<size_t> relativeRules;
    bool valid = false, matchAll = true, requiresAvailability = false, playbackDependent = false;
    int sortField = 0, sortDirection = 0;
    size_t limit = 0;
    int64_t evaluatedAtSec = 0;
};

} // namespace Aperture
