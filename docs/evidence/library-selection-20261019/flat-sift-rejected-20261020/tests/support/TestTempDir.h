#pragma once

#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <random>
#include <chrono>
#include <iostream>
#include <filesystem>

namespace Aperture::Test {

namespace fs = std::filesystem;

class ScopedTempDir {
public:
    ScopedTempDir(const std::wstring& prefix = L"Aperture_Test_") {
        wchar_t tempRoot[MAX_PATH];
        DWORD len = GetTempPathW(MAX_PATH, tempRoot);
        if (len == 0 || len >= MAX_PATH) {
            wcscpy_s(tempRoot, L".");
        }

        auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        std::random_device rd;
        std::mt19937_64 gen(rd() ^ now);
        std::uniform_int_distribution<uint64_t> dis;

        path = std::wstring(tempRoot);
        if (!path.empty() && path.back() != L'\\') {
            path += L'\\';
        }
        path += prefix + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(dis(gen));

        CreateDirectoryW(path.c_str(), NULL);
    }

    ~ScopedTempDir() {
        Cleanup();
    }

    // Move constructor
    ScopedTempDir(ScopedTempDir&& other) noexcept : path(std::move(other.path)), autoCleanup(other.autoCleanup) {
        other.autoCleanup = false;
    }

    // Move assignment
    ScopedTempDir& operator=(ScopedTempDir&& other) noexcept {
        if (this != &other) {
            Cleanup();
            path = std::move(other.path);
            autoCleanup = other.autoCleanup;
            other.autoCleanup = false;
        }
        return *this;
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const std::wstring& GetPath() const {
        return path;
    }

    std::wstring SubPath(const std::wstring& rel) const {
        std::wstring p = path;
        if (!p.empty() && p.back() != L'\\') p += L'\\';
        if (rel.size() >= 2 && rel[0] == L'.' && rel[1] == L'\\') {
            p += rel.substr(2);
        } else {
            p += rel;
        }
        return p;
    }

    bool CreateSubDir(const std::wstring& rel) const {
        std::wstring full = SubPath(rel);
        int res = SHCreateDirectoryExW(NULL, full.c_str(), NULL);
        return (res == ERROR_SUCCESS || res == ERROR_ALREADY_EXISTS || res == ERROR_FILE_EXISTS);
    }

    bool WriteFile(const std::wstring& rel, const std::string& content) const {
        std::wstring full = SubPath(rel);
        size_t lastSlash = full.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            std::wstring dir = full.substr(0, lastSlash);
            SHCreateDirectoryExW(NULL, dir.c_str(), NULL);
        }

        HANDLE hFile = CreateFileW(full.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return false;

        DWORD written = 0;
        BOOL ok = TRUE;
        if (!content.empty()) {
            ok = ::WriteFile(hFile, content.data(), (DWORD)content.size(), &written, NULL) && (written == (DWORD)content.size());
        }
        CloseHandle(hFile);
        return ok != FALSE;
    }

    bool CopyTo(const std::wstring& destRoot) const {
        try {
            fs::copy(fs::path(path), fs::path(destRoot), fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            return true;
        } catch (...) {
            return false;
        }
    }

    void DisableAutoCleanup() {
        autoCleanup = false;
    }

    void Cleanup() {
        if (autoCleanup && !path.empty()) {
            RemoveDirectoryRecursive(path);
            path.clear();
        }
    }

    static bool RemoveDirectoryRecursive(const std::wstring& dir) {
        if (dir.empty()) return true;
        try {
            // Remove readonly attribute if any
            SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
            std::error_code ec;
            for (const auto& entry : fs::recursive_directory_iterator(fs::path(dir), fs::directory_options::skip_permission_denied, ec)) {
                SetFileAttributesW(entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
            }
            fs::remove_all(fs::path(dir), ec);
            return !ec;
        } catch (...) {
            return false;
        }
    }

private:
    std::wstring path;
    bool autoCleanup = true;
};

} // namespace Aperture::Test
