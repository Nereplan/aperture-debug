#pragma once

#include <string>

namespace Aperture {

enum class ErrorDomain { Storage, Playback, Import, Renderer, Ipc, System };

enum class ErrorSeverity { Information, Warning, Error, Fatal };

struct AppError {
    ErrorDomain domain = ErrorDomain::System;
    ErrorSeverity severity = ErrorSeverity::Error;
    int code = 0;
    unsigned long systemError = 0;
    bool recoverable = true;
    std::wstring userMessage, technicalMessage, relatedPath;
    static AppError Storage(int code, unsigned long systemError, const std::wstring& userMessage, const std::wstring& technicalMessage,
                            const std::wstring& relatedPath) {
        AppError error;
        error.domain = ErrorDomain::Storage;
        error.code = code;
        error.systemError = systemError;
        error.userMessage = userMessage;
        error.technicalMessage = technicalMessage;
        error.relatedPath = relatedPath;
        return error;
    }
    static AppError Playback(const std::wstring& message, int code = 0) {
        AppError error;
        error.domain = ErrorDomain::Playback;
        error.code = code;
        error.userMessage = message;
        error.technicalMessage = message;
        return error;
    }
    bool IsValid() const { return !userMessage.empty() || !technicalMessage.empty(); }
};

inline const wchar_t* ErrorDomainName(ErrorDomain domain) {
    switch (domain) {
        case ErrorDomain::Storage: return L"Storage";
        case ErrorDomain::Playback: return L"Playback";
        case ErrorDomain::Import: return L"Import";
        case ErrorDomain::Renderer: return L"Renderer";
        case ErrorDomain::Ipc: return L"IPC";
        case ErrorDomain::System: return L"System";
    }
    return L"System";
}

} // namespace Aperture
