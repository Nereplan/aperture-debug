#pragma once

#include "Common.h"
#include "OperationCancellation.h"

namespace Aperture {

// The interrupt closure shares ownership of a real thread handle, so Cancel
// racing with completion cannot use a closed handle or a reused thread ID.
class Win32IoCancellation {
public:
    explicit Win32IoCancellation(std::shared_ptr<OperationCancellation> cancellation) : cancellation_(std::move(cancellation)) {
        HANDLE handle = nullptr;
        if (cancellation_ && DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            std::shared_ptr<void> owned(handle, [](void* value) { CloseHandle(value); });
            cancellation_->SetInterrupt([owned] { CancelSynchronousIo(owned.get()); });
        }
    }
    ~Win32IoCancellation() { if (cancellation_) cancellation_->ClearInterrupt(); }
    Win32IoCancellation(const Win32IoCancellation&) = delete;
    Win32IoCancellation& operator=(const Win32IoCancellation&) = delete;
private:
    std::shared_ptr<OperationCancellation> cancellation_;
};

} // namespace Aperture
