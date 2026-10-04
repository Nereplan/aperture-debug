# Test efficiency and disk usage

Measured on Windows x64 on 2026-10-04. Audited baseline: `ea0a340`. Final source built, tested, packaged, and installed: `4b30177`. This was local preparation at version `2.0.0.0`; no release was published.

## Results

| Measurement | Before | After | Change |
|---|---:|---:|---:|
| Complete PR regression gate | 454.76 s | 370.26 s | 84.50 s less, 18.58% faster |
| Production compilations across workflows/unit/corpus/persistence | 67 | 27 | 40 duplicate compilations avoided |
| Registered unit cases | 402 | 381 | 21 weak/redundant cases removed |
| Registered persistence cases | 106 | 105 | 1 assertion-free case removed |
| Release rollback storage | 84.61 MiB | 34.18 MiB | 50.43 MiB saved |
| Debug rollback storage | 87.24 MiB | 34.40 MiB | 52.84 MiB saved |
| Complete recovery snapshots per installation | 3 | 3 | Same retained history |
| Packaged ZIP | 13,271,078 bytes | 13,274,144 bytes | +3,066 bytes |
| Release executable | 3,122,688 bytes | 3,125,760 bytes | +3,072 bytes |

Backup savings across the two installations total **103.27 MiB**. The executable and ZIP grew by approximately 3 KiB for the codec logic; the major disk saving comes from eliminating repeated backup payloads.

Gate timings are individual local runs, not controlled benchmark medians. Mutation compilation overlapped part of the final run, so the percentage is an observed result, not a guarantee for every machine. All 20 PR suites passed. The final unit, persistence, IPC, loader, updater, and stress runs reported zero skipped cases.

## Removing tests that did not validate application behavior

The following removals account for all 22 deleted cases. Real renderer, touch, codec, persistence, and duplicate-oracle coverage remains.

| Removed group | Count | Reason |
|---|---:|---|
| RenderViewModel snapshot/default/projection examples | 13 | Assigned or copied local fields and calculated geometry inside the test; did not call the production projection builder |
| HDR metadata projection example | 1 | Reimplemented conversion formulas locally |
| Screenshot live-mpv smoke placeholder | 1 | No assertions and no initialized live player |
| Subtitle safe-area diagnostics projection example | 1 | Asserted fields assigned by the test itself |
| Bulk context-menu disabled-state example | 1 | Assigned the same boolean subsequently asserted |
| Seekbar local hover/clamping example and live-mpv placeholder | 2 | Local formulas or unconditional early return; neither exercised live behavior |
| Bundled locale structural parity and RTL placeholder parity | 2 | Required four catalogs absent from the shipped product and returned without checking them |
| Read-only directory detection | 1 | No production operation/assertion; Windows directory read-only attributes do not enforce write protection |

`SettingsTabSystemActionMatching` retains the meaningful production action matcher and checks both the intended target and a different target at the same index. The real flyout borrowing test remains.

Coverage was strengthened where the case had a useful purpose:

- Video adjustment validation now writes malformed settings to an isolated library and loads them through `MediaLibrary::LoadFromRoot`. It checks rejected out-of-range values, retained valid saturation, and an unknown field surviving the real parser.
- RTL behavior now creates actual Arabic/Hebrew fixture catalogs and initializes `Localization`. It checks returned text, formatting, and RTL policy instead of silently relying on missing bundled files.
- Taskbar icon lifecycle now injects the taskbar mock, publishes real icons, checks shell-sized dimensions with Windows APIs, and verifies destruction.
- Atomic writes to a read-only file must fail with the expected rename error and leave original bytes intact.
- The unit harness supplies the bundled mpv DLL in its isolated directory, so bundled-profile policy coverage actually executes.
- Async network stress now checks that the projection reader runs, snapshots stay consistent, cancellation clears opening state, callbacks stay within the allowed count, and closing clears state.

## Making the harness honest and faster

Six native executables share `tests/support/TestRunner.h`. A selected test that executes no assertions now fails. Unknown arguments, missing filter values, and filters matching no tests fail rather than reporting success. Explicit skips are reported separately; skipping after a failed assertion cannot hide that failure.

`framework_self_test.cpp` verifies 14 runner scenarios, including assertion-free execution, empty selections, skip accounting, eight concurrent failures, evaluating comparison operands once, and keeping temporary container elements alive for the comparison. Assertion recording is synchronized; comparison helpers evaluate each operand once.

The workflow, unit, corpus, and persistence scripts share `tests/support/native_build.ps1`. Their common production files compile once inside a temporary pool owned by the central test invocation. Harness sources always compile fresh. The key includes the compiler binary, compiler path, exact flags, all production source/header contents and paths, and shared test headers. Changed source and mutation trees therefore receive separate objects. Failed builds do not promote objects. The pool is removed on completion, failure, or timeout; no persistent compiler cache was added.

The final gate compiled 12 production files for workflows, 15 additional files for units, and reused all production objects needed by corpus and persistence. The unnecessary corpus version-header generation was also removed.

| Suite | Before | After |
|---|---:|---:|
| Workflows | 39.63 s | 40.66 s |
| Unit | 87.67 s | 62.85 s |
| Corpus | 43.63 s | 6.10 s |
| Persistence | 35.66 s | 8.86 s |

Separate mutation checks deliberately break duration ordering and resume-position serialization. Both now fail through behavioral assertions. The resume mutation modifies both the compact serializer and DOM fallback; the previous target missed the compact path. Compilation failure is explicitly rejected as mutation evidence.

## Smaller thumbnails without lowering JPEG quality

Opaque frames retain JPEG quality `0.80` and full `4:4:4` chroma. The encoder samples at most 512 pixels and tries lossless PNG only for a limited palette or at least 90% repeated neighboring samples. It keeps PNG only when smaller than the already encoded JPEG. Transparent frames keep the existing PNG path. Cache keys, header versions, dimensions, and configured disk budgets stay compatible.

The 90% repetition threshold avoids an extra PNG encode for the tested mixed photographic frames. A candidate graphic needs two encodes: at 320×180 the measured flat-graphic overhead was about 0.8 ms; at 640×360 it was about 3 ms. This is a deliberate background encoding cost for smaller files. Photographic samples retained identical JPEG bytes.

Existing raw/PNG entries migrate through the bounded background worker only when the result is smaller. JPEG entries are never recompressed. A 128-entry in-memory LRU remembers already optimal raw/PNG content using identity, length, and a fingerprint. Repeated reads avoid decoding/re-encoding; changed bytes are checked again, old fingerprints expire, and failed writes remain retryable. It stores no duplicate image payloads or new disk metadata.

`tests/thumbnails/codec_benchmark.cpp` compares the original fixed JPEG/transparent-PNG policy against the adaptive policy. It runs 20 alternating timing rounds after warm-up and validates exact pixels for PNG and exact baseline bytes for JPEG. This run included 21 synthetic fixtures at 160×90, 320×180, and 640×360, plus one decoded existing thumbnail read without modifying the cache.

| Fixture subset | Original bytes | Adaptive bytes | Reduction |
|---|---:|---:|---:|
| 12 flat/palette/checkerboard/subtitle graphics | 218,030 | 12,805 | 94.13% |
| All 22 samples | 660,241 | 455,016 | 31.08% |

Individual graphic fixtures saved approximately 65–98%. The aggregate is dominated by artificial checkerboards and is **not a forecast for a real media library**. Natural-grain, mixed-photo, transparent, and the decoded photographic cache sample had identical encoded sizes. Full per-frame sizes and median times are in [codec.csv](../evidence/size-evidence/codec.csv).

## Sharing rollback payloads

Update backups retain ordinary, complete recovery trees. Their immutable copied payloads now share a content-addressed pool through NTFS hard links. Live executables, DLLs, data, and configuration never alias the pool. Old snapshots, including incomplete recovery trees, are compacted after a successful installation. Read-only saved files retain their copies and attributes.

A repeated install with identical runtime files creates no new backup. Changed packages replace only differing files, allowing an unchanged locked dependency to remain installed. Pool contents are hash-verified before use. Garbage collection removes unreferenced objects while preserving incomplete recovery trees. File systems without hard links fall back to copies; that capability result is remembered for the install, and unused pool objects are removed.

Package tests exercise no-op installation, three retained revisions, sharing with live-file independence, damaged object rejection, old object cleanup, unsupported-hard-link fallback, read-only saved data, partial-update rollback, package tampering, and unsafe paths. The retention tests passed under PowerShell 7 and Windows PowerShell 5.1.

Storage evidence counts unique file identities using Windows `st_dev`/`st_ino` and sums their lengths once. Ordinary folder-size totals double-count hard links; allocated clusters and file-system metadata are excluded. Thus these are distinct payload bytes, not exact free-space deltas. Both installed copies retain three complete snapshots. All 59 pre-recorded profile/cache/configuration files remained byte-identical, and live files were verified independent of the pool.

Raw measurements are in [storage-before.json](../evidence/size-evidence/storage-before.json), [storage-after.json](../evidence/size-evidence/storage-after.json), and [gate-summary.json](../evidence/size-evidence/gate-summary.json). Private profile paths and contents are excluded.

## Reproducing the checks

From the repository root with the installed MSVC x64 toolchain:

```powershell
.\build_and_release.ps1 -FailFast
.\tests\run_tests.ps1 -Id workflow_mutations -FailFast
```

Run these sequentially because some harness reports use fixed diagnostic log paths.

The codec experiment uses an external temporary output directory and optionally reads an existing cache:

```powershell
. .\tests\support\native_build.ps1
$benchmarkRoot = Join-Path ([IO.Path]::GetTempPath()) ('Aperture-codec-benchmark-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $benchmarkRoot | Out-Null
$executable = Join-Path $benchmarkRoot 'codec_benchmark.exe'
Build-ApertureTestHarness -Sources @((Resolve-Path 'tests/thumbnails/codec_benchmark.cpp').Path) -Output $executable -Libraries @('ole32.lib','oleaut32.lib','windowscodecs.lib')
& $executable (Join-Path $benchmarkRoot 'results') (Resolve-Path 'release/cache').Path
if ($LASTEXITCODE -ne 0) { throw 'Codec experiment failed.' }
```
