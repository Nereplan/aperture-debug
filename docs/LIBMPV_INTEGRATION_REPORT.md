# libmpv size build integration

Integrated on 2026-09-30. The current source and installed builds were saved outside the workspace before any changes, including uncommitted work, release/debug profiles, distributions and diagnostics. **No commit was made.**

Backup: the sibling `Aperture Backups/before-libmpv-20260930-071859` directory. The initial snapshot contains 1,145 SHA-256-verified files (375,779,908 bytes). `backup-manifest.json`, `HEAD.txt` and `working-tree.patch` describe the saved state. Verified source archives and the adopted engine were subsequently saved under its separate `dll-rebuild-inputs/` directory.

## Installed artifacts

At integration, both `release/Aperture.exe` and `debug/Aperture.exe` received the tested Release build, as before. Each installation had the new engine and a verified runtime manifest. The portable ZIP is `dist/Aperture-2.0.0.0-windows-x64.zip`. **All 27 personal files matched the external pre-integration backup byte for byte.** The production installer also retained rollback snapshots. The later cleanup below keeps the release available and archives the duplicate debug runtime and rollback snapshots.

| Artifact | Before integration | Integrated build | Reduction |
| --- | ---: | ---: | ---: |
| libmpv DLL | 39,929,344 bytes | 25,240,576 bytes | 36.8% |
| Clean portable runtime | 44,117,876 bytes | 29,429,108 bytes | 33.3% |
| Portable ZIP | 18,636,557 bytes | 12,818,960 bytes | 31.2% |

At engine integration, the application executable was 2,947,584 bytes with SHA-256 `7d26509daf0fc8403166239026aa4047e8956843c45fd360f126d8aa71955611`; matching PDB symbols were archived and verified separately, then moved from `dist/symbols/` to the external cleanup archive. The subsequent fullscreen fix below has a new executable and matching symbols.

Adopted DLL SHA-256: `0fc93966f7e0ff741a60f356152ccb5080e0c538b3d5dcffe001f344222d2efe`. This is a fresh build from the durable recipe. Its size matches the earlier candidate. The dependency inventory and public headers now identify the actual v0.41.0 source tag and API 2.5.

## Rebuild and feature configuration

`scripts/build_lean_libmpv.ps1` now drives the repository's source pins and flags under `scripts/libmpv/`. All eight source download URLs were fetched and verified against the archive hashes in `sources.lock.json`. The fresh build completed successfully; the cached rerun also passed. The cached output differs in four bytes confined to the PE timestamp/checksum fields; all section contents match. The first, fully tested output is the installed DLL.

The recipe builds the shader chain at `-Os`, and FFmpeg/libplacebo/mpv with size optimization and LTO. GCC 16.2's shader-chain LTO compiler error is avoided by retaining ordinary compilation there. Assembly and runtime CPU dispatch remain enabled. The DLL has only Windows system imports and needs no MSYS2 runtime DLLs. Its 19,317,984-byte virtual `.bss` remains zero bytes on disk; `-fdata-sections` is deliberately excluded.

The existing lean codec/container/protocol configuration is retained, with `dynaudnorm` and `pan` added for night mode and channel mapping. D3D11 decoding/rendering, WASAPI, libass, colour management, screenshots and the software-render preview remain available. Optional libdovi is disabled, while FFmpeg Dolby Vision metadata parsing and libplacebo reshaping remain compiled in. The original DLL's precise source revision remains uncertain; its version string/imports are evidence, not proof of a source commit.

The wrapper validates cleanup paths before writes, holds an exclusive build lock, handles spaces through a stable temporary drive alias, honors `-OutputDir`, records the installed MSYS2 packages, and avoids global package updates. It rejects cached dependencies when the recipe or package inventory changes. See [CUSTOM_MPV_BUILD.md](CUSTOM_MPV_BUILD.md) for setup and commands.

## Validation

- All **18 PR regression suites** have passing final outcomes. The initial full run passed 16; the other two were rerun successfully after fixing environment-sensitive fixtures. Unit tests now activate common-controls v6 as the app does and own their tooltip test window. Persistence tests select an absent drive instead of assuming `Q:` is unused. Final counts: **330 unit tests and 105 persistence tests**. Passing suites were not repeated unnecessarily.
- The fresh DLL passed **59 isolated playback checks**: H.264, HEVC, AV1, VP9, MPEG-4 and WMV containers/audio; active D3D11VA and software decoding; HEVC 10-bit HDR/PQ; seek, pause, resume and speed; SRT, ASS/libass and embedded mov_text; PNG thumbnails; software-render frames; night mode, pan and combined filters; downmix option acceptance; loopback HTTP.
- HTTPS, RTSP/TCP and RTMP passed with local fixtures. A normally trusted W3C HTTPS media endpoint also played with certificate verification enabled. The self-signed local HTTPS fixture alone bypasses verification in its test client; the Windows trust store and production settings were unchanged.
- Actual Aperture playback passed CLI/hub open checks and complete seek/pause/queue/resume/reopen workflows: **three restart cycles and 95 commands**. Profiles were isolated from the user's installations.
- Release packaging passed hash, architecture, runtime dependency closure, isolated startup and exact-process shutdown checks. Installed manifest entries and DLL/executable hashes match the tested package. Matching symbols passed archive verification.

The legacy `balance` property is unavailable on both original and new engines. The playback harness records that fact as a diagnostic; it does not count it as a supported feature. The pan-based channel-map filter is tested separately and passes.

## Performance

Measurements use the same 60-second 1080p H.264 file, warm file cache, identical options, isolated DLL copies and fresh harness processes. After one warmup per engine, three alternating pairs were measured. Startup uses three pairs of processes, each with five timed first-frame restart events after an internal warmup.

| Median measurement | Original DLL | New DLL | Change |
| --- | ---: | ---: | ---: |
| Software-decode CPU time | 32.953 s | 33.578 s | +1.9% |
| Software-decode wall time | 2.462 s | 2.510 s | +1.9% |
| D3D11VA wall time | 11.437 s | 11.450 s | +0.1% |
| D3D11VA CPU time | 2.688 s | 2.688 s | 0.0% |
| Startup first-frame event | 0.171 s | 0.170 s | -0.6% |

This sample shows a small software-decoding CPU tradeoff. Hardware decoding and startup changed little. Three pairs do not establish performance for every codec, resolution or device, and the unavailable frame-drop counters cannot certify presentation pacing. HDR display quality and Dolby Vision presentation still require suitable media/hardware and manual inspection.

The earlier benchmark could miss fast completion: it compared a flag's string value with `true` and relied on polling a narrow final position interval. The durable harness now waits for the explicit EOF event and records the hardware decoder before teardown. The measurements above use that corrected harness; earlier scratch timing claims are not substituted for these results.

## Evidence and cleanup

Compact reports, full gate/retest logs, loaded-module playback logs, source verification, PE comparisons, build provenance, installation hashes, screenshots and raw performance runs were collected under `diagnostics/libmpv-integration/`. These records are now retained under `archived/diagnostics/libmpv-integration/` in the sibling `Aperture Backups/commit-preparation-20260930-090430` directory. The original build report and candidate evidence are preserved in the earlier external backup.

Temporary candidate/source/build trees, generated media, test DLL copies and duplicate packages were archived outside the workspace after validation, under the backup's `generated-dll-work/` directory. This moves **3,892,658,817 bytes across 55,276 files** out of the working tree and retains them on disk for reversible recovery. The earlier size-build archive is split between `mpv-size-build/` and `mpv-size-build-remaining/` because a stale loopback HTTP server initially held its media directory open; that verified test server was stopped before cleanup completed.

During commit preparation, the tested file-specific cleanup script removed **435.53 MiB** of regenerable compiler artifacts. Remaining build metadata and the release thumbnail cache were moved outside the workspace. The duplicate debug runtime, installed rollback history, symbol archives and validation records were also archived outside the workspace; 784 moved files were SHA-256 verified. `release/Aperture.exe` and the portable ZIP remain available. Live `debug/data/`, `debug/mpv.conf` and `release/data/` were left in place; **all 44 current personal files remain byte-identical** to the fresh pre-cleanup snapshot.

The full PR regression gate was rerun during this cleanup and passed **18/18 suites in one run**, including 330 unit tests, 105 persistence tests and static analysis. Rebuild scripts and playback fixture generators remain with the source; future builds regenerate their caches. No generated engine build tree is needed to reproduce the configuration.

The leftover `Q:` and `S:` drives were verified as `SUBST` aliases of `D:\`, then removed with `subst /D`. No disk contents were deleted or copied by this operation. The `D:` volume identity and top-level entries were unchanged, and no alias startup entry was found. Source preparation remains on `main`; no commit or push has been made.

## Fullscreen exit follow-up

Fullscreen exit previously restored the normal window bounds and then maximized the window, exposing a visible shrink followed by expansion. Both fullscreen exit and return-to-library now share a direct placement restore: the maximized style is restored before applying a maximized placement, the frame is refreshed after the final bounds are set, and DWM transitions are suppressed during the change.

The regression test failed against the previous implementation and now verifies that every emitted resize uses the final bounds, for normal/maximized targets and both exit paths. All **331 unit tests** pass, including existing PiP round trips. The actual-app workflow passed **106 commands**, including Escape and the overlay chevron from both normal and maximized windows. A clean Release build, package hashes/dependency checks, isolated startup/shutdown and matching symbol archive verification also passed. All **50 current personal files** remained unchanged after installation.

The updated `release/Aperture.exe` is **2,947,072 bytes**, SHA-256 `1c0a6d081fe2c08c0e048d0f9959c185adf0b6e888a45c0edf59c13e9c4bb889`. The refreshed portable ZIP is **12,818,866 bytes**. The libmpv DLL is unchanged. The previous executable/package, current symbols and verification records are retained in the sibling `Aperture Backups/fullscreen-fix-20260930` directory; generated build files and rollback copies remain outside the project. No commit or push was made.
