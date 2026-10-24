#pragma once

// Opt-in, portable-friendly file-type registration under HKCU only (no admin,
// no default hijacking). Aperture.Video is added as an Open With choice and,
// only where no ProgId default exists yet, set as the extension default.
#include <string>

namespace Aperture {

struct FileAssociationResult {
    int registered = 0;   // extensions processed successfully
    int failed = 0;       // extensions with at least one failed registry write
    bool unsupportedOs = false;
};

// Registry key name for a supported extension value: leading dots stripped,
// lowercased (".MKV" -> "mkv"). Pure string logic; unit-tested.
std::wstring FileAssociationExtensionKey(const std::wstring& dotExtension);

FileAssociationResult RegisterFileAssociations();
FileAssociationResult UnregisterFileAssociations();
// True when the Aperture.Video open command exists under HKCU\Software\Classes.
bool GetFileAssociationState();

} // namespace Aperture
