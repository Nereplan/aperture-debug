#pragma once
#include "Json.h"
#include <cmath>

namespace Aperture {
inline bool HasSupportedStoreSchema(const JsonValue& root, int currentVersion) {
    if (!root.is_object()) return false;
    if (!root.has_key("schemaVersion")) return true;
    const auto& version = root.get("schemaVersion");
    const double number = version.as_number(-1);
    return version.is_number() && std::isfinite(number) && number >= 0 &&
        number <= currentVersion && std::floor(number) == number;
}
inline bool HasFutureStoreSchema(const JsonValue& root, int currentVersion) {
    return root.is_object() && root.get("schemaVersion").is_number() &&
        root.get("schemaVersion").as_number(0) > currentVersion;
}
} // namespace Aperture
