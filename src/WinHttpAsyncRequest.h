#pragma once

#include "OperationCancellation.h"
#include <windows.h>
#include <winhttp.h>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace Aperture::HttpDetail {

// WinHTTP only permits cross-thread cancellation for asynchronous requests.
// This adapter waits on the worker, while the cancellation token closes the
// request. HANDLE_CLOSING is the final callback and fences callback-context and
// read-buffer lifetime before the worker releases them.
class AsyncRequest {
    struct State {
        std::mutex handleMutex, completionMutex;
        std::condition_variable changed;
        HINTERNET handle = nullptr;
        DWORD expected = 0, error = 0, bytes = 0;
        bool done = false, closed = false;
        void Close() {
            HINTERNET old;
            { std::lock_guard<std::mutex> lock(handleMutex); old = handle; handle = nullptr; }
            if (old) WinHttpCloseHandle(old);
        }
        template<class Function> bool Use(Function&& function) {
            std::lock_guard<std::mutex> lock(handleMutex);
            if (!handle) { SetLastError(ERROR_WINHTTP_OPERATION_CANCELLED); return false; }
            return function(handle) != FALSE;
        }
    };
public:
    AsyncRequest(HINTERNET handle, std::shared_ptr<OperationCancellation> cancellation)
        : state_(std::make_shared<State>()), cancellation_(std::move(cancellation)) {
        state_->handle = handle;
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(state_.get());
        if (!WinHttpSetOption(handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) return;
        const DWORD notifications = WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE | WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE |
            WINHTTP_CALLBACK_FLAG_READ_COMPLETE | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR | WINHTTP_CALLBACK_FLAG_HANDLES;
        if (WinHttpSetStatusCallback(handle, Callback, notifications, 0) == WINHTTP_INVALID_STATUS_CALLBACK) return;
        registered_ = true;
        cancellation_->SetInterrupt([state = state_] { state->Close(); });
    }
    ~AsyncRequest() {
        cancellation_->ClearInterrupt();
        state_->Close();
        if (registered_) {
            std::unique_lock<std::mutex> lock(state_->completionMutex);
            state_->changed.wait(lock, [this] { return state_->closed; });
        }
    }
    bool IsReady() const { return registered_; }
    DWORD_PTR Context() const { return reinterpret_cast<DWORD_PTR>(state_.get()); }
    template<class Function> bool Use(Function&& function) { return state_->Use(std::forward<Function>(function)); }
    template<class Function> bool Call(DWORD expected, Function&& function, DWORD* bytes = nullptr) {
        { std::lock_guard<std::mutex> lock(state_->completionMutex);
          state_->expected = expected; state_->done = false; state_->error = 0; state_->bytes = 0; }
        if (!state_->Use(std::forward<Function>(function))) {
            const auto error = GetLastError();
            if (error != ERROR_IO_PENDING) { SetLastError(error); return false; }
        }
        std::unique_lock<std::mutex> lock(state_->completionMutex);
        state_->changed.wait(lock, [this] { return state_->done || state_->closed; });
        if (cancellation_->IsCancelled()) { SetLastError(ERROR_WINHTTP_OPERATION_CANCELLED); return false; }
        if (state_->error || state_->closed) {
            SetLastError(state_->error ? state_->error : ERROR_WINHTTP_OPERATION_CANCELLED); return false;
        }
        if (bytes) *bytes = state_->bytes;
        return true;
    }
private:
    static void CALLBACK Callback(HINTERNET, DWORD_PTR context, DWORD status, void* information, DWORD length) {
        if (!context) return;
        auto& state = *reinterpret_cast<State*>(context);
        { std::lock_guard<std::mutex> lock(state.completionMutex);
          if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) state.closed = true;
          else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
              state.error = information && length >= sizeof(WINHTTP_ASYNC_RESULT)
                  ? static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError : ERROR_WINHTTP_INTERNAL_ERROR;
              state.done = true;
          } else if (status == state.expected) {
              if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) state.bytes = length;
              state.done = true;
          }
          // Notify before releasing the mutex: the closing waiter may destroy
          // State as soon as it reacquires this lock after the final callback.
          state.changed.notify_all(); }
    }
    std::shared_ptr<State> state_;
    std::shared_ptr<OperationCancellation> cancellation_;
    bool registered_ = false;
};

} // namespace Aperture::HttpDetail
