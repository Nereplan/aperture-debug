#pragma once

#include <memory>
#include <optional>
#include <utility>

namespace Aperture {

// Optional owning value for large, infrequently present record fields. Copies
// remain independent (including save/undo snapshots); moves transfer ownership.
// Unlike inline optional<T>, absent values occupy only one pointer per record.
template <typename T>
class HeapOptional {
public:
    HeapOptional() noexcept = default;
    HeapOptional(std::nullopt_t) noexcept {}
    explicit HeapOptional(const T& value) : value_(std::make_unique<T>(value)) {}
    explicit HeapOptional(T&& value) : value_(std::make_unique<T>(std::move(value))) {}
    HeapOptional(const HeapOptional& other)
        : value_(other ? std::make_unique<T>(*other) : nullptr) {}
    HeapOptional(HeapOptional&&) noexcept = default;
    HeapOptional& operator=(HeapOptional&&) noexcept = default;
    HeapOptional& operator=(const HeapOptional& other) {
        if (this != &other) {
            if (other) *this = *other;
            else reset();
        }
        return *this;
    }
    HeapOptional& operator=(const T& value) {
        if (value_) *value_ = value;
        else value_ = std::make_unique<T>(value);
        return *this;
    }
    HeapOptional& operator=(T&& value) {
        if (value_) *value_ = std::move(value);
        else value_ = std::make_unique<T>(std::move(value));
        return *this;
    }
    HeapOptional& operator=(std::nullopt_t) noexcept { reset(); return *this; }

    template <typename... Args>
    T& emplace(Args&&... args) {
        value_ = std::make_unique<T>(std::forward<Args>(args)...);
        return *value_;
    }
    void reset() noexcept { value_.reset(); }
    bool has_value() const noexcept { return static_cast<bool>(value_); }
    explicit operator bool() const noexcept { return has_value(); }
    T& operator*() noexcept { return *value_; }
    const T& operator*() const noexcept { return *value_; }
    T* operator->() noexcept { return value_.get(); }
    const T* operator->() const noexcept { return value_.get(); }

private:
    std::unique_ptr<T> value_;
};

} // namespace Aperture
