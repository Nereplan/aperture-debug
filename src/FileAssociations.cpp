#include "FileAssociations.h"

#include "MediaLibrary.h"
#include "PathFileHelpers.h"

#include <windows.h>
#include <shlwapi.h>
#include <cwctype>
#include <set>

namespace Aperture {

namespace {

constexpr wchar_t kProgId[] = L"Aperture.Video";
constexpr wchar_t kClassesKey[] = L"Software\\Classes\\";
constexpr wchar_t kProgIdFriendlyName[] = L"Aperture Video";

std::wstring ProgIdKeyPath() { return std::wstring(kClassesKey) + kProgId; }
std::wstring ExtensionKeyPath(const std::wstring& key) { return std::wstring(kClassesKey) + key; }
std::wstring OpenWithProgidsPath(const std::wstring& key) { return std::wstring(kClassesKey) + key + L"\\OpenWithProgids"; }

// Writes the command data under HKCU\Software\Classes\Aperture.Video. The
// executable path is resolved at call time so a moved portable install can
// re-register itself.
bool WriteProgIdRoot() {
    const std::wstring exePath = GetExePath();
    if (exePath.empty()) return false;
    HKEY progId = nullptr;
    const LONG opened = RegCreateKeyExW(HKEY_CURRENT_USER, ProgIdKeyPath().c_str(), 0, nullptr,
                                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &progId, nullptr);
    if (opened != ERROR_SUCCESS) return false;
    const std::wstring defaultIcon = exePath + L",0";
    const std::wstring openCommand = L"\"" + exePath + L"\" \"%1\"";
    // Each value is written individually so one failure does not skip the rest.
    const LONG friendly = RegSetValueExW(progId, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(kProgIdFriendlyName),
                                         static_cast<DWORD>((wcslen(kProgIdFriendlyName) + 1) * sizeof(wchar_t)));
    const LONG icon = RegSetValueExW(progId, L"DefaultIcon", 0, REG_SZ, reinterpret_cast<const BYTE*>(defaultIcon.c_str()),
                                     static_cast<DWORD>((defaultIcon.size() + 1) * sizeof(wchar_t)));
    const LONG command = RegSetValueExW(progId, L"shell\\open\\command", 0, REG_SZ,
                                        reinterpret_cast<const BYTE*>(openCommand.c_str()),
                                        static_cast<DWORD>((openCommand.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(progId);
    return friendly == ERROR_SUCCESS && icon == ERROR_SUCCESS && command == ERROR_SUCCESS;
}

} // namespace

std::wstring FileAssociationExtensionKey(const std::wstring& dotExtension) {
    size_t start = 0;
    while (start < dotExtension.size() && dotExtension[start] == L'.') ++start;
    std::wstring key = dotExtension.substr(start);
    for (wchar_t& c : key) c = static_cast<wchar_t>(towlower(c));
    return key;
}

FileAssociationResult RegisterFileAssociations() {
    FileAssociationResult result;
    // The ProgId must exist before extensions point at it.
    if (!WriteProgIdRoot()) {
        result.failed = 1;
        return result;
    }
    const std::wstring progIdValue(kProgId);
    for (const std::wstring& dotExtension : MediaLibrary::GetSupportedExtensions()) {
        const std::wstring key = FileAssociationExtensionKey(dotExtension);
        if (key.empty()) continue;
        bool ok = true;
        // Always advertise the ProgId through OpenWithProgids so Aperture
        // shows up in "Open with" without hijacking the current default.
        HKEY openWith = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, OpenWithProgidsPath(key).c_str(), 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &openWith, nullptr) == ERROR_SUCCESS) {
            if (RegSetValueExW(openWith, kProgId, 0, REG_NONE, nullptr, 0) != ERROR_SUCCESS) ok = false;
            RegCloseKey(openWith);
        } else {
            ok = false;
        }
        // Set the extension default ONLY when no ProgId default is present;
        // an existing association belongs to the user's current choice.
        HKEY extKey = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, ExtensionKeyPath(key).c_str(), 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &extKey, nullptr) == ERROR_SUCCESS) {
            wchar_t existing[256] = {};
            DWORD existingBytes = sizeof(existing);
            const LONG type = RegGetValueW(HKEY_CURRENT_USER, ExtensionKeyPath(key).c_str(), nullptr,
                                           RRF_RT_REG_SZ, nullptr, existing, &existingBytes);
            const bool hasDefault = type == ERROR_SUCCESS && existing[0] != L'\0';
            if (!hasDefault &&
                RegSetValueExW(extKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(progIdValue.c_str()),
                               static_cast<DWORD>((progIdValue.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
                ok = false;
            }
            RegCloseKey(extKey);
        } else {
            ok = false;
        }
        if (ok) ++result.registered; else ++result.failed;
    }
    return result;
}

FileAssociationResult UnregisterFileAssociations() {
    FileAssociationResult result;
    // Remove the ProgId tree first; extensions stop advertising Aperture. A
    // missing tree (nothing was ever registered) is not a failure.
    const LONG deletedTree = SHDeleteKeyW(HKEY_CURRENT_USER, ProgIdKeyPath().c_str());
    const bool treeOk = deletedTree == ERROR_SUCCESS || deletedTree == ERROR_FILE_NOT_FOUND;
    if (!treeOk) ++result.failed;
    for (const std::wstring& dotExtension : MediaLibrary::GetSupportedExtensions()) {
        const std::wstring key = FileAssociationExtensionKey(dotExtension);
        if (key.empty()) continue;
        bool ok = true;
        HKEY openWith = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, OpenWithProgidsPath(key).c_str(), 0, KEY_SET_VALUE, &openWith) == ERROR_SUCCESS) {
            const LONG deleted = RegDeleteValueW(openWith, kProgId);
            if (deleted != ERROR_SUCCESS && deleted != ERROR_FILE_NOT_FOUND) ok = false;
            RegCloseKey(openWith);
        }
        HKEY extKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, ExtensionKeyPath(key).c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &extKey) == ERROR_SUCCESS) {
            wchar_t existing[256] = {};
            DWORD existingBytes = sizeof(existing);
            if (RegGetValueW(HKEY_CURRENT_USER, ExtensionKeyPath(key).c_str(), nullptr,
                             RRF_RT_REG_SZ, nullptr, existing, &existingBytes) == ERROR_SUCCESS &&
                _wcsicmp(existing, kProgId) == 0) {
                // Only remove the default when it is ours; never clobber a
                // different association the user may have chosen meanwhile.
                if (RegDeleteValueW(extKey, nullptr) != ERROR_SUCCESS) ok = false;
            }
            RegCloseKey(extKey);
        }
        if (ok) ++result.registered; else ++result.failed;
    }
    return result;
}

bool GetFileAssociationState() {
    HKEY commandKey = nullptr;
    const LONG opened = RegOpenKeyExW(HKEY_CURRENT_USER,
                                      (ProgIdKeyPath() + L"\\shell\\open\\command").c_str(), 0, KEY_READ, &commandKey);
    if (opened == ERROR_SUCCESS) RegCloseKey(commandKey);
    return opened == ERROR_SUCCESS;
}

} // namespace Aperture
