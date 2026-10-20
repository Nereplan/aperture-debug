#pragma once

#include "Error.h"
#include <string>

namespace Aperture::Diagnostics {

bool Initialize(bool loggingEnabled);
void StartWatchdog();
void Shutdown();
void SetLoggingEnabled(bool enabled);
void Log(const AppError& error);
void RecordHeartbeat(const std::wstring& state);
// Timestamp-only by default: system-wide thread enumeration and module snapshots
// must not gate startup. Explicit resource capture is for post-readiness probes.
// Retained in memory (capped) and flushed to diagnostics/perf/ only on request.
void RecordStartupMilestone(const std::wstring& name, bool captureResources = false);
void WriteStartupReport();
} // namespace Aperture::Diagnostics
