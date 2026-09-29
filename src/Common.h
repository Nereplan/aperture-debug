#pragma once

// Compatibility umbrella. The system/graphics include block lives in
// AppIncludes.h and the inline helper groups live in focused leaf headers so
// callers can include exactly what they need; including this header keeps the
// historical single-include behavior for existing translation units.

#include "AppIncludes.h"

#include "AnimationCurves.h"
#include "BackupFiles.h"
#include "CacheStorage.h"
#include "ColorHelpers.h"
#include "CommonStrings.h"
#include "Formatters.h"
#include "GeometryHelpers.h"
#include "PathFileHelpers.h"
#include "ScreenshotPaths.h"
#include "StorageStatus.h"
