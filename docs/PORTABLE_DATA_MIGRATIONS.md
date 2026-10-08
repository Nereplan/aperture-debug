# Aperture Portable Data Model & Migration Reference

## 1. Architectural Invariants & Portable Containment

Aperture is permanently portable-only. All application-owned persistent state, configuration, cached data, diagnostics, and logs reside strictly within the directory hierarchy containing `Aperture.exe`.

### 1.1 Containment Guarantees

1. **Executable-Root Locality**: All durable data is written under `<rootDir>\data\`, disposable caches under `<rootDir>\cache\`, diagnostic telemetry under `<rootDir>\diagnostics\`, and user captures under `<rootDir>\screenshots\`.
2. **Zero External Profile Leakage**: Aperture never writes to `%APPDATA%`, `%LOCALAPPDATA%`, `%USERPROFILE%`, the Windows Registry (except transient shell verbs during explicit `--register-shell` / `--unregister-shell` execution), or external system directories.
3. **No External Fallback on Unwritable Root**: If `<rootDir>` or `<rootDir>\data\` is read-only or unwritable, storage operations fail with typed error codes (`StorageErrorCode::DirectoryNotWritable`, `StorageErrorCode::FileOpenFailed`, or `StorageErrorCode::FileWriteFailed`). Aperture never silently relocates user configuration or library data to external locations.
4. **Media Reference Separation**: Imported external media files and playlists are referenced strictly by path (using root-relative `.\media\...` syntax when inside the portable root, or normalized absolute paths when on external volumes). Aperture never writes sidecar files, metadata overrides, thumbnails, or durable state beside external source media.
5. **Disposable Cache Isolation**: Cache directories under `<rootDir>\cache\` (`thumbnails\`, `shaders\`, `subtitles\`, `generic\`) are strictly segregated from durable data in `<rootDir>\data\`. Clearing caches (`ClearDisposableCache`) never modifies or removes durable library, settings, playlist, or history files.

---

## 2. Storage Primitives & Failure Surface

All durable file writes in Aperture share robust, atomic persistence semantics implemented in `src/Common.h`.

### 2.1 Atomic Writes (`AtomicWriteTextFile`)

Durable files are updated using a two-phase commit:
1. Data is written to a unique sibling temporary file: `<targetPath>.tmp.<pid>.<tid>`.
2. Internal file buffers are flushed to disk via `FlushFileBuffers`.
3. The temporary file is atomically renamed to the destination file using `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`.
4. If writing or flushing fails at any point, the temporary file is deleted and the existing target file remains unmodified.

### 2.2 Rotating Backups (`CreateRotatingBackup`)

Before overwriting critical state files (`settings.json`, `library.json`, `playlists.json`, `history.json`, `watched_folders.json`, `session.json`, `mpv.conf`), Aperture creates rotating backups in `<rootDir>\data\backups\`:
- Backups are indexed from `.bak.1` (most recent) to `.bak.N` (oldest).
- On each save, `.bak.N-1` is moved to `.bak.N`, and the current live file is copied to `.bak.1`.
- The default retention window is 3 backups (up to 5 for settings restoration).

### 2.3 Backup Recovery (`RestoreFromNewestValidBackup`)

When a durable JSON file fails to parse or is empty/corrupt:
1. Aperture sequentially inspects `.bak.1` through `.bak.N`.
2. The candidate backup is parsed and validated against schema constraints (e.g. valid JSON object/array).
3. The first valid backup is loaded into memory, and the load result indicates `recoveredFromBackup = true`.

### 2.4 Typed Persistence Errors (`StorageStatus` / `StorageErrorCode`)

Storage operations return `StorageStatus` containing:
- `code`: Enumerated `StorageErrorCode` (`Success`, `DirectoryCreationFailed`, `DirectoryNotWritable`, `FileOpenFailed`, `FileWriteFailed`, `FileReadFailed`, `AtomicRenameFailed`, `SerializationFailed`, `InvalidPath`, `ReadOnlyMedia`, `EmptyData`, `CacheBudgetExceeded`).
- `win32Error`: Win32 `DWORD` error code (e.g., `ERROR_ACCESS_DENIED`, `ERROR_DISK_FULL`, `ERROR_INVALID_DATA`).
- `path`: Target file or directory path involved.
- `message`: Diagnostic human-readable context.

---

## 3. Versioned Durable Stores

### 3.1 Settings (`data\settings.json`)

- **Authoritative Model**: `Aperture::Settings` (`src/Settings.h`, `src/Settings.cpp`)
- **Current Schema Version**: `15` (`Settings::kCurrentSettingsSchemaVersion`)
- **Root-Relative Path**: `data\settings.json`
- **Migration Symbol**: `Settings::MigrateSchema(JsonValue& root, int fromVersion, int targetVersion)`
- **Backup Mechanism**: `CreateRotatingBackup(path, 3)` into `data\backups\settings.json.bak.1..3`; recovery searches up to `.bak.5`. Last-known-good backup saved to `data\settings.lkg.json`.
- **Private-Mode Behavior**: Settings changes remain in-memory and are saved as normal application configuration.
- **Forward-Version & Unknown Fields**: Unknown top-level fields and unknown properties in `shortcuts` are preserved in `persistedFields` and `persistedShortcutFields`, surviving migration and round-trip saves. If `loadedSchemaVersion > kCurrentSettingsSchemaVersion`, migration is skipped and existing fields are preserved.

#### Version Transition History

| From | To | Added / Modified Fields | Default / Migration Logic |
|:---:|:---:|:---|:---|
| `v0` | `v1` | `volume`, `themePreset`, `accentColor`, `shortcuts` | Legacy unversioned format adopted; defaults assigned if missing. |
| `v1` | `v2` | `audioLang`, `subLang` | Defaulted to `"eng"`. |
| `v2` | `v3` | `storageMode`, `playlistViewMode`, `windowWidth`, `windowHeight`, `windowX`, `windowY`, `windowMaximized` | Defaulted to `"source"`, `"list"`, `1280x720`, centered. |
| `v3` | `v4` | `lastAudioTrackTitle`, `lastSubtitleTrackTitle`, shortcut struct migration | Shortcuts migrated from plain string key names to structured `{ "vk": N, "key": "...", "ctrl": bool, ... }` objects. |
| `v4` | `v5` | `restoreSession` | Defaulted to `true`. |
| `v5` | `v6` | `cacheTotalBudgetMB`, `cacheThumbnailBudgetMB`, `cacheShaderBudgetMB`, `cacheSubtitleBudgetMB`, `cacheGenericBudgetMB` | Defaulted to `160`, `64`, `64`, `16`, `16` MB (normalized total; class sum matches the ceiling). |
| `v6` | `v7` | `hdrMode`, `toneMappingAlgorithm`, `targetPeak`, `gamutMapping`, `iccProfilePath` | Defaulted to `"auto"`, `"auto"`, `0`, `"auto"`, `""`. |
| `v7` | `v8` | `audioChannelMix`, `audioNormalizeDownmix`, `nightMode`, `audioBalance`, `audioChannelMap` | Defaulted to `"auto"`, `true`, `"off"`, `0.0`, `"auto"`. |
| `v8` | `v9` | `subtitleProfiles`, `activeSubtitleProfile`, `subSafeAreaX`, `subSafeAreaY` | Defaulted to `[]`, `""`, `0`, `0`. |
| `v9` | `v10` | `onlineSubtitlesEnabled`, `onlineSubtitleEndpoint` | Defaulted to `false`, `""`. |
| `v10` | `v11` | `customMpvConfigEnabled`, `customMpvConfigPath` | Defaulted to `false`, `"mpv.conf"`. |
| `v11` | `v12` | `jumpListEnabled` | Defaulted to `false`. |
| `v12` | `v13` | `hoverPreviewEnabled`, `reduceMotionEnabled` | Defaulted to `true`, `false`. |
| `v13` | `v14` | `autoUpdateCheckEnabled`, `updateChannel`, `updateCheckUrl` | Defaulted to `false`, `"stable"`, `""`. |
| `v14` | `v15` | `appLanguage`, `locale` | Migrated from legacy `locale` key if present; defaulted to `"en-US"`. |
### 3.2 Media Library (`data\library.json`)

- **Authoritative Model**: `Aperture::MediaLibrary` (`src/MediaLibrary.h`, `src/MediaLibrary.cpp`)
- **Current Schema Version**: `2` (`MediaLibrary::kCurrentLibrarySchemaVersion`)
- **Root-Relative Path**: `data\library.json`
- **Migration Symbol**: `MediaLibrary::MigrateLibrarySchema(JsonValue& root, int fromVersion, int targetVersion)`
- **Backup Mechanism**: `CreateRotatingBackup(libPath, 3)` into `data\backups\library.json.bak.1..3`.
- **Private-Mode Behavior**: Media item state modifications (e.g. progress updates) are suppressed in private mode.
- **Partial Recovery & Fault Tolerance**: If individual media items in the `items` array contain malformed data or missing mandatory fields, only the invalid items are skipped. Valid items are retained and loaded successfully.
- **Forward-Version Handling**: A schema version newer than the application supports is rejected and protected from saving. Unknown properties on media items in supported schemas are preserved.
- **Metadata fingerprint**: The optional `metadataFileWriteTime` field is an exact decimal-string Windows file timestamp. Together with `fileSizeBytes`, `fileIndex` and `volumeSerialNumber`, it allows reuse of successfully probed metadata for an unchanged file. Missing or zero timestamps disable reuse; legacy records require no eager migration or scan. A successful stable probe establishes the timestamp, and failed or changed-file observations invalidate it. This additive field does not change schema version `2`.

#### Version Transition History

| From | To | Transformation |
|:---:|:---:|:---|
| `v0`/`v1` | `v2` | Legacy format was a bare JSON array `[ { ...item } ]`. Migrated to root JSON object `{ "schemaVersion": 2, "items": [ ... ] }`. If root was an empty object without `"items"`, an empty array is created. |

---

### 3.3 Playlists (`data\playlists.json`)

- **Authoritative Model**: `Aperture::MediaLibrary` (`src/MediaLibrary.h`, `src/MediaLibrary.cpp`)
- **Current Schema Version**: `2` (`MediaLibrary::kCurrentPlaylistsSchemaVersion`)
- **Root-Relative Path**: `data\playlists.json`
- **Migration Symbol**: `MediaLibrary::MigratePlaylistsSchema(JsonValue& root, int fromVersion, int targetVersion)`
- **Backup Mechanism**: `CreateRotatingBackup(plPath, 3)` into `data\backups\playlists.json.bak.1..3`.
- **Private-Mode Behavior**: Playlists are persisted as normal user library structures.

#### Version Transition History

| From | To | Transformation |
|:---:|:---:|:---|
| `v0`/`v1` | `v2` | Legacy format was a bare JSON array `[ { ...playlist } ]`. Migrated to root JSON object `{ "schemaVersion": 2, "items": [ ... ] }`. |

---

### 3.4 History (`data\history.json`)

- **Authoritative Model**: `Aperture::MediaLibrary` (`src/MediaLibrary.h`, `src/MediaLibrary.cpp`)
- **Current Schema Version**: `2` (`MediaLibrary::kCurrentHistorySchemaVersion`)
- **Root-Relative Path**: `data\history.json`
- **Migration Symbol**: `MediaLibrary::MigrateHistorySchema(JsonValue& root, int fromVersion, int targetVersion)`
- **Backup Mechanism**: `CreateRotatingBackup(histPath, 3)` into `data\backups\history.json.bak.1..3`.
- **Private-Mode Behavior**: **Strictly suppressed.** In private mode, playback history entries are not appended to the in-memory history list and are not written to `history.json`.
- **Capacity Policy**: Automatically capped to the 100 most recent records; deduplicated by case-insensitive normalized path key.

#### Version Transition History

| From | To | Transformation |
|:---:|:---:|:---|
| `v0`/`v1` | `v2` | Legacy format was a bare JSON array `[ { ...historyItem } ]`. Migrated to root JSON object `{ "schemaVersion": 2, "items": [ ... ] }`. |

---

### 3.5 Watched Folders (`data\watched_folders.json`)

- **Authoritative Model**: `Aperture::MediaLibrary` (`src/MediaLibrary.h`, `src/MediaLibrary.cpp`)
- **Current Schema Version**: `1` (`MediaLibrary::kCurrentWatchedFoldersSchemaVersion`)
- **Root-Relative Path**: `data\watched_folders.json`
- **Migration Symbol**: `MediaLibrary::MigrateWatchedFoldersSchema(JsonValue& root, int fromVersion, int targetVersion)`
- **Backup Mechanism**: `CreateRotatingBackup(wfPath, 3)` into `data\backups\watched_folders.json.bak.1..3`.
- **Path Portability**: Paths located inside `<rootDir>` are serialized with relative prefix `.\<folder>`, ensuring seamless volume/folder relocation.

---

### 3.6 Playback Session (`data\session.json`)

- **Authoritative Model**: `Aperture::PlaybackSession` (`src/PlaybackSession.h`, `src/PlaybackSession.cpp`)
- **Current Schema Version**: `1` (`PlaybackSession::kCurrentSessionSchemaVersion`)
- **Root-Relative Path**: `data\session.json`
- **Migration Symbol**: `PlaybackSession::MigrateSchema(JsonValue& root, int fromVersion, int targetVersion)`
- **Backup Mechanism**: `CreateRotatingBackup(targetPath, 3)` into `data\backups\session.json.bak.1..3`.
- **Private-Mode Behavior**: **Strictly suppressed.** When `isPrivateMode == true`, `PlaybackSession::Save` immediately returns `StorageStatus::Ok()` without writing or touching `session.json`.
- **Forward-Version & Unknown Fields**: Unknown properties are preserved in `persistedFields`.

---

### 3.7 Recent Network Streams (`recent_streams.json`)

- **Authoritative Model**: `Aperture::NetworkStreamModel` (`src/NetworkStream.h`, `src/NetworkStream.cpp`)
- **Current Schema Version**: `1` (`NetworkStreamModel::kCurrentStreamSchemaVersion`)
- **Root-Relative Path**: `recent_streams.json` (`NetworkStreamModel::kDefaultRecentStreamsFileName`)
- **Private-Mode Behavior**: Suppressed when `enableRecentStreams == false` or in private mode.
- **Capacity**: Bounded to `maxRecentStreams` (default 10).

---

### 3.8 Updater State (`data\updater.json`)

- **Authoritative Model**: `Aperture::Updater` (`src/Updater.h`, `src/Updater.cpp`)
- **Current Schema Version**: `1`
- **Root-Relative Path**: `data\updater.json`
- **Private-Mode Behavior**: **Strictly suppressed.** In private mode, automatic update checks and state persistence are disabled; `SaveUpdaterState` returns `StorageStatus::Ok()` immediately without writing to disk.

---

### 3.9 Channel Policy (`channels.json`)

- **Authoritative Model**: `Aperture::ChannelPolicy` (`src/ChannelPolicy.h`, `src/ChannelPolicy.cpp`)
- **Current Schema Version**: `1`
- **Root-Relative Path**: `channels.json`
- **Role**: Read-only distribution asset and local policy definition specifying release channels (`stable`, `beta`, `nightly`), retention days, update cadence, and SemVer prerelease patterns. Validated against `channels.schema.json`.

---

## 4. Non-Versioned Stores & Disposable Assets

The following files are stored in the portable root but are deliberately non-versioned:

| Store / Path | Classification & Rationale | Atomic / Backup Protection |
|:---|:---|:---|
| `data\mpv.conf` | **Plain-text libmpv configuration**. Uses `property=value` syntax parsed directly by libmpv. Not JSON-modeled. | Backed up via `CreateRotatingBackup(3)` before any reset/template write; Last-Known-Good configuration snapshot stored at `data\mpv.conf.lkg`. |
| `data\mpv.conf.lkg` | **Last-Known-Good mpv configuration**. Verified snapshot created when custom mpv configuration loads successfully. | Written atomically via `AtomicWriteTextFile`. |
| `data\diagnostics\aperture.log` (and `.1..5`) | **Rolling runtime telemetry**. Unstructured plain-text logs for diagnostic inspection. | Size-capped at 1 MiB (`kMaxLogBytes`) with rolling 5-file retention (`.1` to `.5`). |
| `data\diagnostics\*.dmp` | **Win32 MiniDump crash / hang dumps**. Binary minidumps written on unhandled exceptions or watchdog timeouts. | Generated via `MiniDumpWriteDump` with unique timestamped filenames (`crash-YYYYMMDD-HHMMSS-mmm.dmp`, `hang-YYYYMMDD-HHMMSS-mmm.dmp`). |
| `data\diagnostics\diagnostics-*.json` | **Point-in-time exported reports**. Immutable diagnostic snapshots generated via BuildInfo export. | Written atomically via `AtomicWriteTextFile`. |
| `cache\thumbnails\*` | **Disposable video thumbnails**. WIC/Direct2D cached bitmaps. | Evicted automatically under LRU `CacheBudgetConfig` (64 MB default, 160 MB total). Safe to delete at any time. |
| `cache\shaders\*` | **Disposable compiled shaders**. Direct3D 11 and mpv GPU shader binaries. | Evicted under LRU `CacheBudgetConfig` (64 MB default, 160 MB total). Safe to delete at any time. |
| `cache\subtitles\*` | **Disposable online downloaded subtitles**. Retrieved SRT/VTT subtitle tracks. | Evicted under LRU `CacheBudgetConfig` (16 MB default, 160 MB total). Safe to delete at any time. |
| `cache\generic\*` | **Disposable auxiliary caches**. Ephemeral network and parser assets. | Evicted under LRU `CacheBudgetConfig` (16 MB default, 160 MB total). Safe to delete at any time. |
| `screenshots\*.png` | **User frame captures**. Visual media captures initiated by user action. | Stored with sanitized, timestamped filenames (`<stem>_YYYYMMDD_HHMMSS_<index>.png`). |

---

## 5. Operator Recovery Procedures

### 5.1 Restoring from Rotating Backups

If a persistent store becomes corrupt due to external process interference:
1. Terminate `Aperture.exe`.
2. Inspect `<rootDir>\data\backups\`.
3. Rename the desired backup (e.g. `data\backups\library.json.bak.1`) over the live target (`data\library.json`).
4. Restart `Aperture.exe`.

### 5.2 Restoring Default or Last-Known-Good `mpv.conf`

- **Via UI**: In Settings &rarr; Playback &rarr; Custom mpv Configuration, click **Revert to Last Known Good** or **Reset to Safe Defaults**.
- **Manual**: Copy `<rootDir>\data\mpv.conf.lkg` to `<rootDir>\data\mpv.conf`, or delete `mpv.conf` (Aperture will recreate default safe template on next edit/launch).

### 5.3 Cache Maintenance & Reset

To reclaim disk space or purge corrupt temporary assets:
- Delete the entire `<rootDir>\cache\` directory or run `ClearDisposableCache()`.
- Aperture automatically recreates the cache class subdirectories on next startup with zero data loss to settings, library, or playlists.

---

## 6. Machine-Checked Verification

To prevent schema and path drift between code, documentation, and manifests:
- The machine-readable manifest is stored at `docs/portable_data_manifest.json`.
- The persistence test suite (`tests/persistence/TestSchemaMigration.h`, executed via `tests/persistence/run_tests.ps1 -Suite SchemaMigration`) compiles directly against authoritative C++ schema constants (`Settings::kCurrentSettingsSchemaVersion`, `MediaLibrary::kCurrentLibrarySchemaVersion`, etc.) and asserts:
  1. Every documented version matches the compiled constant exactly.
  2. Every store is mapped without duplicates or missing entries.
  3. Step-by-step migration chains execute deterministically for all versions.
  4. Unknown fields and forward versions survive round-trip serialization.
  5. Private-mode suppression strictly prevents disk mutation for sensitive stores.
