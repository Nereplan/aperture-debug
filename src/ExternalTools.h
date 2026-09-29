#pragma once

#include <string>

namespace Aperture {

struct ExternalToolDiscoveryResult { bool found = false, rootLocal = false; std::wstring executablePath, displayPath, error; };

class ExternalTools {
public:
    // External PATH probing is opt-in. Root-local candidates are always safe to inspect and create no durable state.
    static ExternalToolDiscoveryResult DiscoverYtDlp(bool allowExternalPath);
};

} // namespace Aperture
