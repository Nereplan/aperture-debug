cmake_minimum_required(VERSION 3.20...3.30)

# Read authoritative single version source from version.json
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.json" APERTURE_VERSION_JSON_DATA)
string(JSON APERTURE_VERSION_MAJOR GET "${APERTURE_VERSION_JSON_DATA}" "major")
string(JSON APERTURE_VERSION_MINOR GET "${APERTURE_VERSION_JSON_DATA}" "minor")
string(JSON APERTURE_VERSION_PATCH GET "${APERTURE_VERSION_JSON_DATA}" "patch")
string(JSON APERTURE_VERSION_BUILD GET "${APERTURE_VERSION_JSON_DATA}" "build")
string(JSON APERTURE_VERSION_STRING GET "${APERTURE_VERSION_JSON_DATA}" "versionString")
string(JSON APERTURE_DISPLAY_VERSION GET "${APERTURE_VERSION_JSON_DATA}" "displayVersion")
string(JSON APERTURE_COMPANY_NAME GET "${APERTURE_VERSION_JSON_DATA}" "companyName")
string(JSON APERTURE_FILE_DESCRIPTION GET "${APERTURE_VERSION_JSON_DATA}" "fileDescription")
string(JSON APERTURE_FILE_VERSION GET "${APERTURE_VERSION_JSON_DATA}" "fileVersion")
string(JSON APERTURE_INTERNAL_NAME GET "${APERTURE_VERSION_JSON_DATA}" "internalName")
string(JSON APERTURE_LEGAL_COPYRIGHT GET "${APERTURE_VERSION_JSON_DATA}" "legalCopyright")
string(JSON APERTURE_ORIGINAL_FILENAME GET "${APERTURE_VERSION_JSON_DATA}" "originalFilename")
string(JSON APERTURE_PRODUCT_NAME GET "${APERTURE_VERSION_JSON_DATA}" "productName")
string(JSON APERTURE_PRODUCT_VERSION GET "${APERTURE_VERSION_JSON_DATA}" "productVersion")
string(JSON APERTURE_COMMENTS GET "${APERTURE_VERSION_JSON_DATA}" "comments")
string(JSON APERTURE_LANG_ID_INT GET "${APERTURE_VERSION_JSON_DATA}" "languageId")
string(JSON APERTURE_CODEPAGE_ID_INT GET "${APERTURE_VERSION_JSON_DATA}" "codePageId")
string(JSON APERTURE_LANG_HEX GET "${APERTURE_VERSION_JSON_DATA}" "language")
string(JSON APERTURE_CODEPAGE_HEX GET "${APERTURE_VERSION_JSON_DATA}" "codePage")
set(APERTURE_TRANSLATION_STRING "${APERTURE_LANG_HEX}${APERTURE_CODEPAGE_HEX}")

set(APERTURE_VERSION_CSV "${APERTURE_VERSION_MAJOR},${APERTURE_VERSION_MINOR},${APERTURE_VERSION_PATCH},${APERTURE_VERSION_BUILD}")
set(APERTURE_PROJECT_VERSION "${APERTURE_VERSION_MAJOR}.${APERTURE_VERSION_MINOR}.${APERTURE_VERSION_PATCH}.${APERTURE_VERSION_BUILD}")

project(Aperture
    VERSION ${APERTURE_PROJECT_VERSION}
    DESCRIPTION "${APERTURE_FILE_DESCRIPTION}"
    LANGUAGES CXX RC
)

# Generated build artifacts directory
set(APERTURE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${APERTURE_GENERATED_DIR}")

set(APERTURE_APP_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/app.ico")
set(APERTURE_APP_MANIFEST_PATH "${APERTURE_GENERATED_DIR}/app.manifest")

# Generate authoritative version header, app manifest, and Windows resource file
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in"
    "${APERTURE_GENERATED_DIR}/Version.h"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest.in"
    "${APERTURE_GENERATED_DIR}/app.manifest"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/resources.rc.in"
    "${APERTURE_GENERATED_DIR}/resources.rc"
    @ONLY
)
# Standard C++17 settings
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Supported build configurations: Release, Debug, Sanitize
set(CMAKE_CONFIGURATION_TYPES "Release;Debug;Sanitize" CACHE STRING "Supported configurations" FORCE)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Release" "Debug" "Sanitize")
endif()
# Static analysis configuration
option(APERTURE_ENABLE_STATIC_ANALYSIS "Enable MSVC native static code analysis (/analyze)" OFF)
option(APERTURE_STATIC_ANALYSIS_WX "Treat static analysis warnings as errors (/analyze:WX)" OFF)
# Build-time provisioned updater trust configuration (immutable release trust root)
set(APERTURE_UPDATER_MANIFEST_URL "" CACHE STRING "Release manifest HTTPS URL for production self-updates")
set(APERTURE_UPDATER_ALLOWED_HOSTS "" CACHE STRING "Semicolon-separated allowlisted hosts for update downloads")
set(APERTURE_UPDATER_PUBLIC_KEY_HEX "" CACHE STRING "Hex-encoded 72-byte ECDSA P-256 public key blob for manifest verification")
set(APERTURE_UPDATER_SIGNER_THUMBPRINT "" CACHE STRING "Hex-encoded SHA-256 Authenticode leaf certificate thumbprint")
set(APERTURE_UPDATER_STABLE_MANIFEST_URL "" CACHE STRING "Release manifest HTTPS URL for stable channel updates")
set(APERTURE_UPDATER_BETA_MANIFEST_URL "" CACHE STRING "Release manifest HTTPS URL for beta channel updates")
set(APERTURE_UPDATER_NIGHTLY_MANIFEST_URL "" CACHE STRING "Release manifest HTTPS URL for nightly channel updates")
set(APERTURE_UPDATER_STABLE_PUBLIC_KEY_HEX "" CACHE STRING "Hex-encoded public key for stable channel verification")
set(APERTURE_UPDATER_BETA_PUBLIC_KEY_HEX "" CACHE STRING "Hex-encoded public key for beta channel verification")
set(APERTURE_UPDATER_NIGHTLY_PUBLIC_KEY_HEX "" CACHE STRING "Hex-encoded public key for nightly channel verification")
set(APERTURE_UPDATER_STABLE_SIGNER_THUMBPRINT "" CACHE STRING "Signer thumbprint for stable channel Authenticode verification")
set(APERTURE_UPDATER_BETA_SIGNER_THUMBPRINT "" CACHE STRING "Signer thumbprint for beta channel Authenticode verification")
set(APERTURE_UPDATER_NIGHTLY_SIGNER_THUMBPRINT "" CACHE STRING "Signer thumbprint for nightly channel Authenticode verification")


# Source files
set(APERTURE_SOURCES
    src/Accessibility.cpp
    src/App.cpp
    src/AppAccessibility.cpp
    src/AppDragDrop.cpp
    src/AppFileOps.cpp
    src/AppHover.cpp
    src/AppImportJobs.cpp
    src/AppKeyboard.cpp
    src/AppLibrary.cpp
    src/AppMenus.cpp
    src/AppModals.cpp
    src/AppPersistence.cpp
    src/AppPlayback.cpp
    src/AppSelection.cpp
    src/AppTooltips.cpp
    src/AppTouch.cpp
    src/AppViewModels.cpp
    src/AudioConfig.cpp
    src/ChannelPolicy.cpp
    src/CommandDispatcher.cpp
    src/Diagnostics.cpp
    src/FolderWatcher.cpp
    src/GlobalSearch.cpp
    src/HttpTransport.cpp
    src/Json.cpp
    src/LibraryQuery.cpp
    src/LibrarySnapshot.cpp
    src/Localization.cpp
    src/MediaLibrary.cpp
    src/MediaFoundation.cpp
    src/MediaProbeIsolation.cpp
    src/MediaTransport.cpp
    src/MpvPlayer.cpp
    src/NetworkStream.cpp
    src/OnlineSubtitles.cpp
    src/OverlayRenderer.cpp
    src/PlaybackSession.cpp
    src/PlaylistIO.cpp
    src/Renderer.cpp
    src/Settings.cpp
    src/SubtitleDiscovery.cpp
    src/Subtitles.cpp
    src/SystemVisualPreferences.cpp
    src/TaskbarController.cpp
    src/TextInputModel.cpp
    src/TooltipManager.cpp
    src/UIComponents.cpp
    src/Updater.cpp
    src/main.cpp
)

# Header files (for IDE visibility and change tracking)
set(APERTURE_HEADERS
    src/Accessibility.h
    src/App.h
    src/AppInternals.h
    src/AppLayout.h
    src/AudioConfig.h
    src/ChannelPolicy.h
    src/CommandDispatcher.h
    src/Common.h
    src/Diagnostics.h
    src/DllLoader.h
    src/Error.h
    src/FolderWatcher.h
    src/GlobalSearch.h
    src/HttpTransport.h
    src/IpcProtocol.h
    src/Json.h
    src/LibraryQuery.h
    src/Localization.h
    src/MediaLibrary.h
    src/MediaFoundation.h
    src/MediaTransport.h
    src/MpvPlayer.h
    src/MpvApi.h
    src/LibrarySaveComparison.h
    src/LibrarySaveWorker.h
    src/MediaProbeIsolation.h
    src/AsyncOwnedJob.h
    src/OperationCancellation.h
    src/OnlineSubtitleJobs.h
    src/WinHttpAsyncRequest.h
    src/OverlayFramePacer.h
    src/NetworkStream.h
    src/OnlineSubtitles.h
    src/OverlayRenderer.h
    src/PlaybackSession.h
    src/PlaylistIO.h
    src/RenderViewModel.h
    src/ChapterMarkerCache.h
    src/Renderer.h
    src/resource.h
    src/Settings.h
    src/SubtitleDiscovery.h
    src/Subtitles.h
    src/SystemVisualPreferences.h
    src/TaskbarController.h
    src/TextInputModel.h
    src/TooltipManager.h
    src/UIComponents.h
    src/Updater.h
    src/WindowGeometry.h
    "${APERTURE_GENERATED_DIR}/Version.h"
)

# Resource files (embeds app.ico, version, and app.manifest with longPathAware)
set(APERTURE_RESOURCES
    "${APERTURE_GENERATED_DIR}/resources.rc"
)

# Executable target
add_executable(Aperture WIN32
    ${APERTURE_SOURCES}
    ${APERTURE_HEADERS}
    ${APERTURE_RESOURCES}
)

# Include directories
target_include_directories(Aperture PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${APERTURE_GENERATED_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Compile definitions
target_compile_definitions(Aperture PRIVATE
    UNICODE
    _UNICODE
    APERTURE_UPDATER_MANIFEST_URL="${APERTURE_UPDATER_MANIFEST_URL}"
    APERTURE_UPDATER_STABLE_MANIFEST_URL="${APERTURE_UPDATER_STABLE_MANIFEST_URL}"
    APERTURE_UPDATER_BETA_MANIFEST_URL="${APERTURE_UPDATER_BETA_MANIFEST_URL}"
    APERTURE_UPDATER_NIGHTLY_MANIFEST_URL="${APERTURE_UPDATER_NIGHTLY_MANIFEST_URL}"
    APERTURE_UPDATER_ALLOWED_HOSTS="${APERTURE_UPDATER_ALLOWED_HOSTS}"
    APERTURE_UPDATER_PUBLIC_KEY_HEX="${APERTURE_UPDATER_PUBLIC_KEY_HEX}"
    APERTURE_UPDATER_STABLE_PUBLIC_KEY_HEX="${APERTURE_UPDATER_STABLE_PUBLIC_KEY_HEX}"
    APERTURE_UPDATER_BETA_PUBLIC_KEY_HEX="${APERTURE_UPDATER_BETA_PUBLIC_KEY_HEX}"
    APERTURE_UPDATER_NIGHTLY_PUBLIC_KEY_HEX="${APERTURE_UPDATER_NIGHTLY_PUBLIC_KEY_HEX}"
    APERTURE_UPDATER_SIGNER_THUMBPRINT="${APERTURE_UPDATER_SIGNER_THUMBPRINT}"
    APERTURE_UPDATER_STABLE_SIGNER_THUMBPRINT="${APERTURE_UPDATER_STABLE_SIGNER_THUMBPRINT}"
    APERTURE_UPDATER_BETA_SIGNER_THUMBPRINT="${APERTURE_UPDATER_BETA_SIGNER_THUMBPRINT}"
    APERTURE_UPDATER_NIGHTLY_SIGNER_THUMBPRINT="${APERTURE_UPDATER_NIGHTLY_SIGNER_THUMBPRINT}"
)

# MSVC Runtime Library mappings
# Release: /MT (Static)
# Debug: /MDd (Dynamic Debug)
# Sanitize: /MD (Dynamic Release with ASan runtime)
set_property(TARGET Aperture PROPERTY
    MSVC_RUNTIME_LIBRARY "$<$<CONFIG:Debug>:MultiThreadedDebugDLL>$<$<CONFIG:Sanitize>:MultiThreadedDLL>$<$<NOT:$<OR:$<CONFIG:Debug>,$<CONFIG:Sanitize>>>:MultiThreaded>"
)

# MSVC Compiler flags and hardening
# Note: "SHELL:/Qspectre" passes /Qspectre compiler codegen directly without triggering
# MSBuild MSB8040 optional Spectre-mitigated CRT library validation.
if(MSVC)
    # CMake's Release default includes /O2. Select the size profile there so
    # MSVC does not compile every translation unit with conflicting switches.
    string(REPLACE "/O2" "/O1" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    target_compile_options(Aperture PRIVATE
        /utf-8
        /EHsc
        /GS
        /sdl
        # Native UI lambdas can exceed the COFF section limit in Debug/ASan builds.
        /bigobj
        $<$<CONFIG:Release>:
            /GL
            /Gy
            /Zi
            /guard:cf
            /Brepro
            "SHELL:/Qspectre"
        >
        $<$<CONFIG:Debug>:
            /Od
            /Zi
            /RTC1
            /guard:cf
            /Brepro
            "SHELL:/Qspectre"
        >
        $<$<CONFIG:Sanitize>:
            /Od
            /Zi
            /fsanitize=address
        >
        $<$<AND:$<BOOL:${APERTURE_ENABLE_STATIC_ANALYSIS}>,$<BOOL:${MSVC}>>:
            /analyze
            /analyze:external-
            /external:anglebrackets
            /external:W0
            $<$<BOOL:${APERTURE_STATIC_ANALYSIS_WX}>:/analyze:WX>
        >
    )

    # MSVC Linker flags, subsystem, and hardening
    target_link_options(Aperture PRIVATE
        /SUBSYSTEM:WINDOWS
        /DYNAMICBASE
        /HIGHENTROPYVA
        /NXCOMPAT
        # ASan's runtime DLL is staged beside the diagnostic executable. The
        # production loader hardening flag would restrict dependent DLL lookup
        # to System32 and prevent that diagnostic runtime from loading.
        $<$<NOT:$<CONFIG:Sanitize>>:/DEPENDENTLOADFLAG:0x800>
        /MANIFEST:NO
        $<$<CONFIG:Release>:
            /DEBUG
            /LTCG
            /OPT:REF
            /OPT:ICF
            /guard:cf
            /CETCOMPAT
            /Brepro
            "/PDBALTPATH:%_PDB%"
        >
        $<$<CONFIG:Debug>:
            /DEBUG
            /guard:cf
            /CETCOMPAT
            /Brepro
        >
        $<$<CONFIG:Sanitize>:
            /DEBUG
            /INCREMENTAL:NO
        >
    )
endif()

# Win32 / Direct2D / MediaFoundation / System libraries
target_link_libraries(Aperture PRIVATE
    d2d1
    dwrite
    d3d11
    dxgi
    windowscodecs
    mfreadwrite
    mfplat
    mfuuid
    dwmapi
    shell32
    shlwapi
    comctl32
    comdlg32
    uxtheme
    user32
    gdi32
    ole32
    oleaut32
    oleacc
    advapi32
    propsys
    dxguid
    imm32
    dbghelp
    runtimeobject
    winhttp
    bcrypt
    crypt32
    wintrust
    psapi
    delayimp
)

# Cold-path delay-load: these DLLs are demonstrably unnecessary
# for the GDI bootstrap shell and the idle hub. Core shell dependencies
# (d2d1, dwrite, d3d11, dxgi, windowscodecs, dwmapi, shell32, shlwapi,
# comctl32, uxtheme, user32, gdi32, ole32, oleaut32, oleacc, advapi32,
# propsys, imm32, runtimeobject, psapi) are NEVER delayed. Every delayed
# call site fails gracefully via SEH guards (see MediaFoundation,
# Diagnostics::WriteProcessDump, HttpTransport, Updater, App file dialogs).
if(MSVC)
    target_link_options(Aperture PRIVATE
        "/DELAYLOAD:MFReadWrite.dll"
        "/DELAYLOAD:MFPlat.DLL"
        "/DELAYLOAD:WinHTTP.dll"
        "/DELAYLOAD:Crypt32.dll"
        "/DELAYLOAD:WinTrust.dll"
        "/DELAYLOAD:bcrypt.dll"
        "/DELAYLOAD:Dbghelp.dll"
        "/DELAYLOAD:ComDlg32.dll"
    )
endif()
