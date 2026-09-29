#pragma once

// Storage & persistence status types shared by every durable-write subsystem.

#include <cstdint>
#include <optional>
#include <string>

namespace Aperture {

// --- Storage & Persistence Status Types ---
enum class StorageErrorCode {
    Success = 0, DirectoryCreationFailed, DirectoryNotWritable, FileOpenFailed, FileWriteFailed, FileReadFailed, AtomicRenameFailed,
    SerializationFailed, InvalidPath, ReadOnlyMedia, EmptyData, CacheBudgetExceeded
};
enum class LongOperationOutcome { Completed, Canceled, Failed };
struct StorageStatus {
    bool success = true;
    StorageErrorCode code = StorageErrorCode::Success;
    DWORD win32Error = 0;
    std::wstring path;
    std::string message;
    static StorageStatus Ok(const std::wstring& p = L"") {
        StorageStatus s;
        s.success = true;
        s.code = StorageErrorCode::Success;
        s.win32Error = 0;
        s.path = p;
        s.message = "OK";
        return s;
    }
    static StorageStatus Error(StorageErrorCode c, DWORD win32Err, const std::wstring& p, const std::string& msg = "") {
        StorageStatus s;
        s.success = false;
        s.code = c;
        s.win32Error = win32Err;
        s.path = p;
        s.message = msg;
        return s;
    }
    bool is_ok() const { return success; }
    explicit operator bool() const { return success; }
};

} // namespace Aperture
