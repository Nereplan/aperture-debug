# Aperture Application Threat Boundaries & Security Architecture

## 1. Architectural Security Overview & Portable Containment

Aperture is a native Windows media player built with C++17, Win32, Direct2D, Direct3D 11, and libmpv. Aperture is permanently **portable-only**: all persistent state, configuration, cached data, diagnostic logs, and crash dumps reside strictly within the directory tree containing `Aperture.exe`.

```
<rootDir>\
  ├── Aperture.exe                  # Primary application executable
  ├── libmpv-2.dll                  # Isolated local playback engine
  ├── vulkan-1.dll                  # Local Vulkan loader
  ├── fonts\                        # Private font assets (Inter, Space Grotesk, JetBrains Mono)
  ├── locales\                      # Localized string catalogs (*.json)
  ├── data\                         # Durable configuration & library stores (atomic writes, rotating backups)
  │   ├── settings.json
  │   ├── library.json
  │   ├── playlists.json
  │   ├── history.json
  │   ├── updater.json
  │   ├── recent_streams.json
  │   ├── mpv.conf
  │   └── backups\                  # Rotating backups (.bak.1 .. .bak.3)
  ├── cache\                        # Disposable caches (thumbnails, shaders, subtitles)
  ├── diagnostics\                  # Local error logs, startup reports, process crash/hang minidumps
  └── screenshots\                  # User-initiated media frame captures
```

### 1.1 Containment Guarantees & Invariant Rules

1. **Executable-Root Locality**: All durable configuration resides under `<rootDir>\data\`, disposable caches under `<rootDir>\cache\`, diagnostic logs and dumps under `<rootDir>\diagnostics\`, and user captures under `<rootDir>\screenshots\`.
2. **Zero External Profile Leakage**: Aperture never writes to `%APPDATA%`, `%LOCALAPPDATA%`, `%USERPROFILE%`, or external system directories.
3. **No External Fallback on Unwritable Root**: If `<rootDir>` is unwritable or read-only, storage operations fail with typed error codes (`StorageErrorCode::DirectoryNotWritable`). Aperture never silently redirects or relocates user data to `%APPDATA%` or user profiles.
4. **Shell Registration Distinction**: The Windows Registry is accessed strictly during explicit `--register-shell` or `--unregister-shell` execution (and user-initiated Settings actions) to register per-user file associations under `HKEY_CURRENT_USER\Software\Classes`. This is Windows-owned shell verb metadata only; Aperture stores **zero** application settings, playback state, or library data in the Windows Registry.
5. **Private-Mode Disk Suppression**: In private mode, history updates, session restoration records, updater check states, and recent network stream lists are retained in-memory and suppressed from disk writes.
6. **No Remote Telemetry**: Aperture transmits zero diagnostic, crash, or analytics data over the network. Logs and minidumps remain strictly local within `<rootDir>\diagnostics\`.

---

## 2. Threat Boundaries Reference

The system defines twelve distinct security boundaries spanning external data, inter-process communication, dynamic library loading, subprocess execution, network operations, persistence, and operating system integration.

```
                    ┌──────────────────────────────────────────────────────────┐
                    │                    TRUST BOUNDARIES                      │
                    └──────────────────────────────────────────────────────────┘
                                                 │
          ┌──────────────────────────────────────┼──────────────────────────────────────┐
          │                                      │                                      │
 ┌─────────────────┐                    ┌─────────────────┐                    ┌─────────────────┐
 │ External Data   │                    │  IPC & Runtime  │                    │ OS & Storage    │
 ├─────────────────┤                    ├─────────────────┤                    ├─────────────────┤
 │ • Media & Cues  │                    │ • Same-User IPC │                    │ • Root Storage  │
 │ • Playlists     │                    │ • DLL Loading   │                    │ • Diagnostics   │
 │ • URLs & Streams│                    │ • External Tools│                    │ • Shell Verbs   │
 │ • Online Subs   │                    │ • UI Drag/Drop  │                    │ • Locales/mpv   │
 │ • Update Pkg    │                    │                 │                    │                 │
 └─────────────────┘                    └─────────────────┘                    └─────────────────┘
```

---

### TB-01: Updater, Cryptographic Verification, and Release Packaging Trust

- **Boundary ID**: `updater_and_package_trust`
- **Category**: `updater_and_packaging`
- **Assets & Actors**: Release manifests (`ReleaseManifest`), downloadable update packages (`.zip` / staged binaries), update check endpoints, Authenticode signing certificates, ECDSA P-256 manifest signatures.
- **Trust Level**: Untrusted remote network inputs and endpoints; pinned cryptographic trust.
- **Entry Points**: `Aperture::CheckForUpdates`, `Aperture::DownloadAndStageUpdate`, `Aperture::ValidateReleaseManifest`, `Aperture::LaunchHelperAndExit`, `Aperture::RunHelperMode` (CLI: `--update-helper`).
- **Security Validations & Bounds**:
  - Manifest size capped at `maxManifestSizeBytes = 128 KiB` (`131072` bytes); package size capped at `maxPackageSizeBytes = 150 MiB` (`157286400` bytes).
  - Monotonic SemVer version progression (`SemanticVersion`); downgrade prevention enforced (`allowDowngrade = false`).
  - Channel stability rank validation and prerelease pattern matching via `ChannelPolicy`.
  - Manifest timestamp validity checks: `publishedUtc <= now <= expiryUtc`; manifests with future timestamps (> 1 hour skew) or expired timestamps are rejected.
  - Manifest detached signature validation using NIST P-256 ECDSA over canonical manifest bytes (`VerifyEcdsaP256Signature`) against pinned public key (`BuildTimeTrustConfig::GetPinnedPublicKeyHex`).
  - Package content SHA-256 digest validation (`ComputeSha256Hex`).
  - Authenticode code signature validation via `WinVerifyTrust` with pinned leaf certificate SHA-256 thumbprint matching (`VerifyAuthenticodeAndPin`).
  - PE header architecture validation enforcing strict x64 machine type `IMAGE_FILE_MACHINE_AMD64` (`0x8664`) via `VerifyPeArchitectureX64`.
  - Network transport bounds: `connectTimeoutMs = 5000`, `downloadTimeoutMs = 60000`, `maxRedirects = 3`.
- **Privilege & Context**: Standard user process. Update helper (`--update-helper`) runs in child process context, verifies parent process PID, stages files in a unique root-local directory (`<rootDir>\cache\updater-tmp\` via `ScopedTempDirectory`), and performs atomic two-phase file swap with automatic rollback on failure (`SwapFailed` -> `RollbackSucceeded`).
- **Durable-Write Destination**: `<rootDir>\data\updater.json` (state record only; suppressed in private mode); staged files swapped into `<rootDir>\`.
- **Failure Behavior**: Returns typed `UpdaterStatusCode`; leaves existing application binary unmodified. Staging directory is cleaned up by RAII guards.
- **Residual Risk**: Compromise of release signing private keys or user-explicit execution in unpinned test override mode.
- **Authoritative Code References**: `src/Updater.h`, `src/Updater.cpp`, `src/ChannelPolicy.h`, `src/ChannelPolicy.cpp`, `scripts/sign_artifacts.ps1`, `scripts/verify_package.ps1`.
- **Authoritative Test References**: `tests/updater/TestManifestValidation.h`, `tests/updater/TestCryptoSignature.h`, `tests/updater/TestAuthenticodePe.h`, `tests/updater/TestSwapRollback.h`, `tests/updater/TestHelperValidation.h`, `tests/updater/TestNetworkBounds.h`, `tests/updater/TestPrivateModeIsolation.h`, `tests/updater/TestChannelPolicy.h`, `tests/updater/TestTempCleanup.h`, `tests/package/test_package_signing.py`.

---

### TB-02: Same-User Single-Instance IPC & Playback Automation

- **Boundary ID**: `same_user_ipc_and_automation`
- **Category**: `ipc_and_automation`
- **Assets & Actors**: External desktop processes, secondary Aperture CLI instances, same-user automation scripts transmitting `WM_COPYDATA` messages.
- **Trust Level**: Same-user desktop processes (untrusted cross-process input within the interactive logon session).
- **Entry Points**: `WindowProc` in `src/main.cpp` (`WM_COPYDATA`), `Ipc::DeserializeEnvelope`, `Ipc::DeserializeAutomationEnvelope`, `Ipc::ParseCommandLine`, `App::ExecuteCommandLineOptions`, `App::ExecuteAutomationCommand`.
- **Security Validations & Bounds**:
  - Magic identifier verification: `IPC_MAGIC_V2 = 0x41505456` (`'APTV'`).
  - Total envelope payload size bounded to `MAX_IPC_PAYLOAD_SIZE = 2 MiB` (`2097152` bytes).
  - File count capped to `MAX_IPC_FILE_COUNT = 1024`.
  - Individual path string length capped to `MAX_IPC_PATH_CHARS = 32768` (Win32 long path maximum).
  - Header checksum verification via Fletcher-32 (`ComputeChecksum`).
  - Process token authentication (`IsSameUserAndSession`): queries `OpenProcessToken` of sender PID, validates identical `TokenSessionId`, and verifies matching user security identifiers (`TokenUser` SID equality via `EqualSid`).
  - Replay protection filter (`ReplayFilter::ValidateAndRecord`): validates timestamps against maximum future clock skew (`kMaxFutureClockSkewMs = 60000`) and maximum age (`kMaxAgeMs = 300000`), rejecting duplicate signatures.
  - Command bounds enforcement: volume values bounded `[0.0 .. 100.0]`, seek positions validated against duration.
- **Privilege & Context**: Standard user interactive session. Unelevated or cross-user senders rejected by token inspection.
- **Durable-Write Destination**: None directly (in-memory state update; normal session persistence on clean exit).
- **Failure Behavior**: Returns `FALSE` from `WindowProc`, dropping malformed or unauthorized payloads without processing.
- **Residual Risk**: A hostile process already running under the identical user account and session token could send valid playback control commands.
- **Authoritative Code References**: `src/IpcProtocol.h`, `src/main.cpp`, `src/App.h`, `src/App.cpp`.
- **Authoritative Test References**: `tests/ipc/TestIpcEnvelope.h`.

---

### TB-03: Dynamic Library Loading & Process Search Path Hardening

- **Boundary ID**: `dll_loading_and_runtime_isolation`
- **Category**: `dll_and_runtime_isolation`
- **Assets & Actors**: Native dynamically linked libraries (`libmpv-2.dll`, `vulkan-1.dll`, Windows System32 libraries).
- **Trust Level**: Local filesystem binaries; untrusted Current Working Directory (CWD) and PATH search order.
- **Entry Points**: `Aperture::DllLoader::HardenProcessDllSearch`, `Aperture::DllLoader::LoadPackageLocalDll`, `Aperture::MpvPlayer::InitializeMpv`.
- **Security Validations & Bounds**:
  - `SetDllDirectoryW(L"")` invoked on startup to remove the current working directory from the DLL search order, preventing CWD binary planting.
  - `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS)` restricts process-wide resolution to application root and System32.
  - Package-local DLL paths strictly resolved against `GetExeDir()` via `ResolvePackageDllPath`, stripping leading separators to prevent directory escape.
  - File existence and regular file attribute verification before invoking `LoadLibraryExW`.
- **Privilege & Context**: Standard user process.
- **Durable-Write Destination**: None (read-only execution).
- **Failure Behavior**: `LoadPackageLocalDll` returns `NULL`; `MpvPlayer::InitializeMpv` reports `MpvInitResult::DllNotFound` or `MpvInitResult::SymbolNotFound` and falls back safely (UI displays static thumbnail, no crash).
- **Residual Risk**: An attacker with write permissions to the application directory could overwrite `libmpv-2.dll` or `vulkan-1.dll`.
- **Authoritative Code References**: `src/DllLoader.h`, `src/MpvPlayer.h`, `src/MpvPlayer.cpp`, `src/main.cpp`.
- **Authoritative Test References**: `tests/loader/TestDllLoader.h`, `tests/loader/run_tests.ps1`, `tests/package/test_pe_dependencies.py`.

---

### TB-04: External Tool Execution Disabled

- **Boundary ID**: `external_tool_execution`
- **Category**: `external_tools`
- **Assets & Actors**: External utility binaries and script handlers.
- **Trust Level**: External executables located on local disk or system PATH.
- **Entry Points**: `Aperture::MpvPlayer::IsCustomMpvOptionSafe`.
- **Security Validations & Bounds**:
  - Direct validated URLs are handled in process. mpv's `ytdl` hook is disabled.
  - Custom options that enable scripts, Lua, or external tools are rejected.
  - There is no external-tool discovery action or subprocess execution path.
- **Privilege & Context**: Standard user process; no external child process.
- **Durable-Write Destination**: None.
- **Failure Behavior**: Unsafe custom mpv options are rejected by the existing configuration validation.
- **Residual Risk**: Media is decoded in process under the playback engine's existing bounds.
- **Authoritative Code References**: `src/MpvPlayer.h`, `src/MpvPlayer.cpp`.
- **Authoritative Test References**: `tests/unit/TestCustomMpvConfig.h`.

---

### TB-05: Network Stream Validation & URL Parsing Boundary

- **Boundary ID**: `network_streams_and_urls`
- **Category**: `network_streams`
- **Assets & Actors**: Network streaming URLs (HTTP, HTTPS, RTSP, RTMP), clipboard URL text, dropped URI links.
- **Trust Level**: Untrusted remote network inputs.
- **Entry Points**: `NetworkStreamValidator::ValidateAndParse`, `NetworkStreamValidator::ExtractAndValidateFromDataObject`, `App::OpenAndPlayMedia`.
- **Security Validations & Bounds**:
  - Maximum URL length bounded to `maxLength = 2048` characters.
  - Control characters (`\0` to `\x1F`, `\x7F`) rejected.
  - Strict scheme whitelist: `http`, `https`, `rtsp`, `rtmp`.
  - Host validation: non-empty, contains only valid DNS/IPv4/IPv6 characters.
  - Port range validated: `[1 .. 65535]`.
  - Embedded credentials policy: `allowCredentials = false` by default; URLs containing `user:pass@host` are rejected or sanitized.
  - Display URL redaction: `GetDisplayUrl(redactCredentials = true)` ensures passwords are masked in UI and logs.
- **Privilege & Context**: Standard user process.
- **Durable-Write Destination**: `<rootDir>\data\recent_streams.json` (bounded stream history; suppressed in private mode).
- **Failure Behavior**: Returns typed `NetworkStreamValidationError`; displays safe OSD notification; aborts playback safely.
- **Residual Risk**: Hostile remote streaming server exploiting vulnerabilities in underlying libmpv / FFmpeg demuxers.
- **Authoritative Code References**: `src/NetworkStream.h`, `src/NetworkStream.cpp`, `src/HttpTransport.h`, `src/HttpTransport.cpp`.
- **Authoritative Test References**: `tests/unit/TestNetworkStream.h`, `tests/corpus/validate_corpus.py`.

---

### TB-06: Online Subtitle Search & Download Boundary

- **Boundary ID**: `online_subtitles_service`
- **Category**: `online_services`
- **Assets & Actors**: Third-party REST API endpoints (e.g. OpenSubtitles), downloaded subtitle payloads (`.srt`, `.vtt`, `.ass`).
- **Trust Level**: Untrusted third-party HTTP/HTTPS endpoints and file downloads.
- **Entry Points**: `OnlineSubtitleService::SearchSubtitles`, `OnlineSubtitleService::DownloadSubtitle`, `OnlineSubtitleService::DownloadAndSaveSubtitle`.
- **Security Validations & Bounds**:
  - Strict HTTPS transport required (`HttpsRequired`).
  - Search query length capped to `kMaxQueryLength = 128` characters.
  - Search results list capped to `kMaxSearchResults = 50` items.
  - Subtitle download file size capped to `kMaxSubtitleFileBytes = 4 MiB` (`4194304` bytes).
  - Network timeouts bounded: connect `10000 ms`, read `15000 ms`, redirects max `3`.
  - Content validation: `SubtitleDiscovery::ValidateSubtitleContent` validates text encoding and structure.
  - Destination containment: downloaded subtitles stored strictly in disposable cache `<rootDir>\cache\subtitles\`.
- **Privilege & Context**: Standard user process.
- **Durable-Write Destination**: Disposable cache `<rootDir>\cache\subtitles\<sanitized_name>` (segregated from durable `data\`).
- **Failure Behavior**: Returns typed `OnlineSubtitleStatus`; errors logged locally; media playback continues uninterrupted.
- **Residual Risk**: Malicious subtitle content designed to trigger memory safety bugs in text layout or libass rendering.
- **Authoritative Code References**: `src/OnlineSubtitles.h`, `src/OnlineSubtitles.cpp`, `src/HttpTransport.h`, `src/HttpTransport.cpp`.
- **Authoritative Test References**: `tests/unit/TestOnlineSubtitles.h`.

---

### TB-07: Media Container, Playlist, and Subtitle File Parsing

- **Boundary ID**: `media_containers_and_playlist_parsing`
- **Category**: `media_and_parsers`
- **Assets & Actors**: Local or network media files (`.mp4`, `.mkv`, `.webm`, `.avi`, `.mov`, `.flac`, `.mp3`), playlists (`.m3u`, `.m3u8`, `.pls`, `.json`), sidecar subtitles (`.srt`, `.vtt`, `.ass`).
- **Trust Level**: Untrusted user-supplied files.
- **Entry Points**: `PlaylistIO::ParsePlaylistFile`, `PlaylistIO::ParsePlaylistContent`, `PlaylistIO::ExportPlaylistFile`, `SubtitleEngine::LoadSubtitleFile`, `SubtitleDiscovery::ValidateSubtitleContent`, `MediaLibrary::ImportPaths`.
- **Security Validations & Bounds**:
  - Playlist file size limit: `maxFileSizeBytes = 10 MiB` (`10485760` bytes).
  - Playlist line count limit: `maxLineCount = 100000`; item count limit: `maxItemCount = 10000`.
  - Playlist line length limit: `maxLineLength = 32768` characters.
  - Subtitle input file size limit: `kMaxSubtitleInputBytes = 32 MiB` (`33554432` bytes).
  - Subtitle cue count limit: `kMaxSubtitleCueCount = 250000`.
  - Subtitle cue text size limit: `kMaxCueTextBytes = 128 KiB` (`131072` bytes).
  - Subtitle tag nesting depth limit: `kMaxFormattingNestingDepth = 32`.
  - Subtitle formatting work units limit: `kMaxFormattingWorkUnits = 65536`.
  - File extension whitelist enforced via `MediaLibrary::GetSupportedExtensions` and `MediaLibrary::IsSupportedExtension`.
  - Non-destructive media access: Aperture reads source media strictly in read-only mode and never creates sidecars beside external files.
- **Privilege & Context**: Standard user process; background worker threads with cooperative cancellation.
- **Durable-Write Destination**: `<rootDir>\data\library.json`, `<rootDir>\data\playlists.json` (metadata index only).
- **Failure Behavior**: Returns typed `PlaylistIoErrorCode` / `StorageStatus`; malformed entries skipped; parsing continues or returns clean error.
- **Residual Risk**: Zero-day vulnerability in native media demuxers (FFmpeg/libmpv) when parsing malformed container bitstreams.
- **Authoritative Code References**: `src/PlaylistIO.h`, `src/PlaylistIO.cpp`, `src/Subtitles.h`, `src/Subtitles.cpp`, `src/SubtitleDiscovery.h`, `src/SubtitleDiscovery.cpp`, `src/MediaLibrary.h`, `src/MediaLibrary.cpp`.
- **Authoritative Test References**: `tests/corpus/validate_corpus.py`, `tests/unit/TestSubtitlesDual.h`, `tests/unit/TestSubtitleDiscovery.h`, `tests/fuzz/run_subtitle_fuzz.ps1`.

---

### TB-08: Portable Storage Containment & Durable JSON Persistence

- **Boundary ID**: `portable_storage_and_root_containment`
- **Category**: `persistence_and_storage`
- **Assets & Actors**: Persistent JSON stores (`settings.json`, `library.json`, `playlists.json`, `history.json`, `watched_folders.json`, `session.json`, `updater.json`, `recent_streams.json`, `mpv.conf`).
- **Trust Level**: Application executable-root filesystem (portable containment).
- **Entry Points**: `AtomicWriteTextFile`, `CreateRotatingBackup`, `RestoreFromNewestValidBackup`, `Settings::Save`, `Settings::Load`, `Settings::MigrateSchema`, `MediaLibrary::SaveLibrary`, `MediaLibrary::SavePlaylists`, `MediaLibrary::SaveHistory`.
- **Security Validations & Bounds**:
  - Strict directory segregation: `<rootDir>\data\` (durable), `<rootDir>\cache\` (disposable), `<rootDir>\diagnostics\` (logs/dumps), `<rootDir>\screenshots\` (captures).
  - Two-phase commit atomic writes: data written to unique temporary sibling `<targetPath>.tmp.<pid>.<tid>`, flushed via `FlushFileBuffers`, and atomically committed via `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`.
  - Rotating backups: creates up to 3 rotating backups (`.bak.1 .. .bak.3`) in `<rootDir>\data\backups\` before overwriting.
  - JSON parser safety limits: maximum nesting depth `kMaxJsonDepth = 64`, maximum string length `kMaxJsonStringLength = 16 MiB`.
  - Forward-version compatibility: preserves unknown JSON fields in `persistedFields` to prevent data loss across version shifts.
  - Private mode enforcement: suppresses disk writes for history, session, updater, and recent streams.
  - Unwritable root containment: returns typed `StorageErrorCode::DirectoryNotWritable` without fallback to external user profiles.
- **Privilege & Context**: Standard user filesystem permissions within `<rootDir>`.
- **Durable-Write Destination**: Strictly `<rootDir>\data\...`.
- **Failure Behavior**: Returns typed `StorageStatus`; existing target files remain unmodified if write/flush/rename fails.
- **Residual Risk**: Hardware disk failure, power loss mid-rename, or unauthorized external process modification of files in `<rootDir>\data\`.
- **Authoritative Code References**: `src/Common.h`, `src/Settings.h`, `src/Settings.cpp`, `src/MediaLibrary.h`, `src/MediaLibrary.cpp`, `src/Json.h`, `src/Json.cpp`.
- **Authoritative Test References**: `tests/persistence/TestSchemaMigration.h`, `tests/persistence/TestUnwritableRoot.h`, `tests/persistence/TestRotatingBackups.h`, `tests/persistence/TestPartialRecovery.h`, `tests/backend/main.cpp`, `tests/persistence/TestRelativePaths.h`, `tests/persistence/TestPortableRoot.h`, `tests/persistence/TestCacheIsolation.h`.

---

### TB-09: Diagnostics, Local Logging, and Crash/Hang Dumps

- **Boundary ID**: `diagnostics_and_process_dumps`
- **Category**: `diagnostics_and_telemetry`
- **Assets & Actors**: Application error logs (`aperture.log`), startup reports (`perf/startup-*.json`), process crash minidumps (`crash-*.dmp`), live hang minidumps (`hang-*.dmp`).
- **Trust Level**: Process memory, exception context, and diagnostic event streams.
- **Entry Points**: `Diagnostics::Initialize`, `Diagnostics::Shutdown`, `Diagnostics::StartWatchdog`, `Diagnostics::Log`, `Diagnostics::RecordHeartbeat`, unhandled exception filter (`WriteCrashDump`).
- **Security Validations & Bounds**:
  - Directory confinement: all diagnostic artifacts written strictly to `<rootDir>\diagnostics\`.
  - Log rotation: active log size capped at `kMaxLogBytes = 1 MiB` (`1048576` bytes); retains `kRetainedLogs = 3` rotating backups (`aperture.log.1 .. 3`).
  - Minidump scope: uses `MiniDumpWithThreadInfo` for minimal crash minidumps, capturing only thread stacks and exception records rather than full process address space.
  - Watchdog heartbeat: polls every `5000 ms`; triggers hang dump only when main thread heartbeat stalls past `kHangThresholdMilliseconds = 30000 ms`.
  - Startup reports written atomically via `AtomicWriteTextFile`.
  - Zero remote transmission: logs and dumps remain strictly local on disk.
- **Privilege & Context**: Standard user process.
- **Durable-Write Destination**: Strictly `<rootDir>\diagnostics\...`.
- **Failure Behavior**: Directory creation failure or write failure disables logging gracefully without interrupting media playback.
- **Residual Risk**: Crash minidumps contain memory fragments that may include user file paths or in-memory metadata present at the time of crash.
- **Authoritative Code References**: `src/Diagnostics.h`, `src/Diagnostics.cpp`, `src/Error.h`.
- **Authoritative Test References**: `tests/diagnostics/diagnostics_test.cpp`, `tests/diagnostics/run_tests.ps1`.

---

### TB-10: Windows Shell Verb Registration & Taskbar Integration

- **Boundary ID**: `shell_registration_and_system_metadata`
- **Category**: `shell_and_os_integration`
- **Assets & Actors**: Windows Shell file associations and JumpLists (removed), plus transient taskbar playback controls.
- **Trust Level**: Windows taskbar COM interface (transient UI state only).
- **Entry Points**: `TaskbarController::Update`, `TaskbarController::Initialize`.
- **Security Validations & Bounds**:
  - Shell registration and the unused JumpList controller have been removed. Taskbar controls store no durable state outside the application root (no `HKCU`, no `%APPDATA%`).
  - The legacy `jumpListEnabled` setting remains readable/writable for profile compatibility and has no shell implementation.
  - Media is opened in-app via file picker, drag/drop, or CLI.
- **Privilege & Context**: Standard user process (no registry writes).
- **Durable-Write Destination**: None (strict portable containment; zero writes outside the application root).
- **Failure Behavior**: Taskbar failures are reported through HRESULT/bool results; no shell registration API exists.
- **Residual Risk**: None from shell writes (disabled). Media must be opened in-app rather than via OS file verbs.
- **Authoritative Code References**: `src/App.h`, `src/App.cpp`, `src/TaskbarController.h`, `src/TaskbarController.cpp`.
- **Authoritative Test References**: `tests/unit/TestTaskbarController.h`.

---

### TB-11: Localization Catalogs & Custom mpv Configuration Filtering

- **Boundary ID**: `localization_and_mpv_configuration`
- **Category**: `configuration_and_localization`
- **Assets & Actors**: Localization string catalogs (`<rootDir>\locales\*.json`), custom mpv configuration file (`<rootDir>\data\mpv.conf`).
- **Trust Level**: Local user-supplied or package-supplied configuration and translation files.
- **Entry Points**: `Localization::Initialize`, `Localization::SetLocale`, `Localization::DiscoverLocales`, `Localization::IsValidLocaleTag`, `MpvPlayer::ParseCustomMpvConfig`, `MpvPlayer::ApplyCustomMpvOptions`.
- **Security Validations & Bounds**:
  - Locale tag validation (`IsValidLocaleTag`): maximum length `32` characters; alphanumeric with `-` and `_` only; strictly rejects path traversal characters (`..`, `/`, `\`, `:`, `?`, `*`).
  - Locales directory confinement: discovery restricted to `<rootDir>\locales\`.
  - Unicode Bidirectional Isolation: `BidiIsolateLtr` (`\x2066` / `\x2069`) and `BidiIsolateRtl` (`\x2067` / `\x2069`) isolate dynamic LTR strings (paths, URLs, numbers) inside RTL interfaces, preventing text spoofing.
  - Custom mpv configuration size bounds: file size capped to `64 KiB` (`65536` bytes), line length capped to `1024` characters, total options capped to `128`.
  - Custom mpv security blacklist (`IsCustomMpvOptionSafe`): strictly blocks:
    - Code execution & scripting: `script`, `scripts`, `script-opts`, `lua`, `js`, `javascript`, `load-scripts`.
    - IPC & input control: `ipc`, `input-ipc-server`, `input-ipc-client`, `input-conf`, `input-terminal`.
    - Config recursion & profiling: `include`, `config`, `config-dir`, `load-config`, `use-filedir-conf`, `profile`.
    - Arbitrary storage dumps: `log-file`, `record-file`, `stream-dump`, `dump-stats`, `screenshot-directory`, `o`, `of`.
    - Network & TLS hijacking: `http-proxy`, `ytdl`, `ytdl-raw-options`, `tls-ca-file`, `tls-cert-file`, `tls-key-file`, `cookies-file`.
    - Host integration & DLL hijacking: `wid`, `force-window-position`, `dll`, `vo-null`, `ao-null`.
- **Privilege & Context**: Standard user process.
- **Durable-Write Destination**: `<rootDir>\data\mpv.conf` (if user edited); `<rootDir>\locales\*.json` read-only.
- **Failure Behavior**: Unsafe mpv options rejected with line-numbered warnings and omitted from player configuration; invalid locale tags fall back to canonical English.
- **Residual Risk**: Safe video/audio shader options causing elevated GPU/CPU resource utilization.
- **Authoritative Code References**: `src/Localization.h`, `src/Localization.cpp`, `src/MpvPlayer.h`, `src/MpvPlayer.cpp`, `src/Settings.h`.
- **Authoritative Test References**: `tests/unit/TestLocalization.h`, `tests/unit/TestRtlComplexScripts.h`, `tests/unit/TestCustomMpvConfig.h`.

---

### TB-12: UI Drag & Drop and Clipboard Input Boundary

- **Boundary ID**: `ui_drag_drop_and_clipboard`
- **Category**: `ui_and_desktop_inputs`
- **Assets & Actors**: OLE drag-and-drop data object (`IDataObject`, `CF_HDROP`, `CF_UNICODETEXT`, URI formats), Windows clipboard (`GetClipboardData`, `SetClipboardData`).
- **Trust Level**: Untrusted desktop window manager and clipboard transfers from external applications.
- **Entry Points**: `ApertureDropTarget::DragEnter`, `ApertureDropTarget::Drop`, `App::OnOleDrop`, `App::HandleDroppedFiles`, `App::MakeClipboardCallbacks`, `TextInputModel::InsertText`, `TextInputModel::HandleCtrlShortcut`.
- **Security Validations & Bounds**:
  - OLE format negotiation: supports `CF_HDROP`, `CF_UNICODETEXT`, `text/uri-list`, `text/plain`.
  - Dropped file path resolution: bounded by `Ipc::ResolveFullPathLong` (`MAX_IPC_PATH_CHARS = 32768`).
  - Dropped URL validation: parsed and sanitized via `NetworkStreamValidator::ExtractAndValidateFromDataObject`.
  - Clipboard text handling: UTF-16 surrogate pair boundary safety (`SafeWideBoundary`, `WideToText16`) prevents malformed surrogate crashes.
  - IME composition safety: composition string validation via `ImmGetCompositionStringW`.
  - Bounded text selection and replacement in `TextInputModel`.
- **Privilege & Context**: Standard user desktop interaction.
- **Durable-Write Destination**: None directly (dispatches in-memory to playback queue or active text field).
- **Failure Behavior**: Invalid drop objects or clipboard formats are safely ignored without UI freezing or unhandled exceptions.
- **Residual Risk**: User inadvertently dropping an untrusted media file into the active player window triggering media demuxing.
- **Authoritative Code References**: `src/App.h`, `src/App.cpp`, `src/TextInputModel.h`, `src/TextInputModel.cpp`, `src/NetworkStream.h`.
- **Authoritative Test References**: `tests/unit/TestNetworkStream.h`.

---

## 3. Threat Boundary Matrix & Verification Summary

| ID | Boundary Name | Trust Level | Key Validations & Bounds | Write Target | Failure Code | Authoritative Tests |
|:---|:---|:---|:---|:---|:---|:---|
| `updater_and_package_trust` | Self-Updater & Packaging | Untrusted Network / Pinned Crypto | Manifest <=128 KiB, Pkg <=150 MiB, NIST P-256 ECDSA, Authenticode, PE x64 | `data/updater.json`, root swap | `UpdaterStatusCode` | `tests/updater/run_tests.ps1`, `tests/package/test_package_signing.py` |
| `same_user_ipc_and_automation` | Same-User IPC & Automation | Same-User Process | `IPC_MAGIC_V2`, Payload <=2 MiB, Token SID & Session Match, Fletcher-32, Replay Filter | None (in-memory) | `Ipc::ValidationResult` | `tests/ipc/TestIpcEnvelope.h` |
| `dll_loading_and_runtime_isolation` | DLL Loader Hardening | Local Filesystem / Untrusted CWD | `SetDllDirectoryW(L"")`, `LOAD_LIBRARY_SEARCH_APPLICATION_DIR`, Root Resolution | None (read-only) | `MpvInitResult` | `tests/loader/TestDllLoader.h`, `tests/package/test_pe_dependencies.py` |
| `external_tool_execution` | External Tools Disabled | External Binaries | ytdl disabled, scripts and tool options blocked | None | Unsafe option rejected | `tests/unit/TestCustomMpvConfig.h` |
| `network_streams_and_urls` | Network Stream URLs | Untrusted Remote URLs | Length <=2048, Scheme Whitelist (`http/https/rtsp/rtmp`), Credential Redaction | `data/recent_streams.json` | `NetworkStreamValidationError` | `tests/unit/TestNetworkStream.h`, `tests/corpus/validate_corpus.py` |
| `online_subtitles_service` | Online Subtitle Service | Untrusted Third-Party APIs | HTTPS enforced, Query <=128, File <=4 MiB, Content Validation | `cache/subtitles/` | `OnlineSubtitleStatus` | `tests/unit/TestOnlineSubtitles.h` |
| `media_containers_and_playlist_parsing` | Media & Playlist Parsers | Untrusted User Media | Playlist <=10 MiB, Subtitle <=32 MiB, Cue <=128 KiB, Tag Nesting <=32 | `data/library.json`, `data/playlists.json` | `PlaylistIoErrorCode` | `tests/corpus/validate_corpus.py`, `tests/unit/TestSubtitlesDual.h` |
| `portable_storage_and_root_containment` | Portable Storage & Containment | Executable-Root Local | Atomic 2-phase commit, 3 rotating backups, JSON depth <=64, private mode suppression | `data/*.json` | `StorageErrorCode` | `tests/persistence/TestSchemaMigration.h`, `tests/persistence/TestUnwritableRoot.h` |
| `diagnostics_and_process_dumps` | Local Diagnostics & Dumps | Process Memory / Exception State | Log <=1 MiB, 3 backups, `MiniDumpWithThreadInfo`, zero remote telemetry | `diagnostics/` | `AppError` | `tests/diagnostics/diagnostics_test.cpp`, `tests/diagnostics/run_tests.ps1` |
| `shell_registration_and_system_metadata` | Windows Shell Registration | Windows Shell Interface (unused) | Disabled no-ops, zero outside writes | None (portable containment) | `bool` | `tests/unit/TestTaskbarController.h` |
| `localization_and_mpv_configuration` | Locales & mpv Config | Local User/Package Config | Locale tag <=32 (no `..`), mpv conf <=64 KiB, 128 options max, strict blacklist | `data/mpv.conf`, `locales/*.json` | `CustomMpvParseResult::errors` | `tests/unit/TestLocalization.h`, `tests/unit/TestCustomMpvConfig.h` |
| `ui_drag_drop_and_clipboard` | UI Drag/Drop & Clipboard | Untrusted Window Manager | Format validation (`CF_HDROP`), surrogate pair boundary safety, URL sanitization | None (in-memory) | `HRESULT` / `NetworkStreamValidationError` | `tests/unit/TestNetworkStream.h` |

---

## 4. Machine Validation & Anti-Drift Policy

To prevent architectural drift between code, documentation, and manifests:
- The authoritative machine-readable manifest is maintained at `docs/threat_boundaries_manifest.json` and validated by `docs/threat_boundaries_manifest.schema.json`.
- The threat boundaries validator (`scripts/validate_threat_boundaries.py`, registered as suite `threat_boundaries` in `tests/suites.json`) automatically verifies:
  1. Manifest schema validity and ID uniqueness.
  2. Existence of all declared `sourceFiles` and `testFiles` on disk.
  3. Code-symbol existence across the codebase for all declared `authoritativeSymbols`.
  4. 1:1 parity between documented boundaries in `docs/THREAT_BOUNDARIES.md` and the machine-readable manifest.
  5. Enforcement of portable storage invariants: rejection of any claims of installed mode, `%APPDATA%`, `%LOCALAPPDATA%`, `%USERPROFILE%`, or storing application state in the Windows Registry.
  6. Rejection of any operational secret keys or private certificates embedded in documentation.
