# Backend performance balance — 17 October 2026

This pass optimizes opening/responding speed, RAM/GPU memory relative to delivered
performance, and portable runtime size. It follows the research recommendations
with measurements rather than adopting every experimental setting. Frontend
behavior and appearance are outside this change.

The baseline is the working tree captured at the start of this pass, on top of
`8fabe56`, including changes already present in that working tree. It is not a
clean checkout of that commit. The matched baseline executable, DLL and relevant
source hashes are recorded in [source identity](../evidence/backend-balance-20261017/source-identity.json).
The backend implementation is committed as `1f50dda`; unrelated pre-existing
working-tree changes were preserved and excluded from that commit.
Results below apply to this Windows machine and these fixtures, not every codec,
GPU, display or competitor configuration. Hardware: Ryzen 9 3900X, RTX 4070 SUPER,
164 Hz reporting, Windows 11 build 26200; [machine record](../evidence/backend-balance-20261017/machine.json).

## Implemented changes

1. **Explicit opens and imports:** after the initial GPU shell and graphics-device
   worker finish, main-player initialization can overlap the library worker when
   an explicit media open is pending. Ordinary hub launches remain lazy. The first
   sorted import target can start before the entire metadata-probe batch finishes.
   A one-shot handoff rejects directory-discovery order, cancellation and stale
   playback intent. Import completion extends the queue without reopening the
   playing file or resetting its session. The import owns that file's metadata
   probe, avoiding a duplicate backfill probe. Unused prepared engines are released
   if loading/importing fails. These changes remove dependencies from the opening
   path; the tests establish ordering and behavior, not a universal latency gain.
2. **Saved startup order:** the library worker prepares the actual saved sort,
   direction and custom folder order. The UI adopts the matching query cache,
   avoiding the previous Title/Ascending plan followed by a second sort. The test
   checks results and zero subsequent query-plan builds.
3. **Smaller engine:** SPIRV-Cross Metal, C++, reflection and utility implementations
   are removed from compilation and the static link closure. Core/C/GLSL/HLSL
   remain because the D3D11 HLSL path needs them. The DLL falls from **25,240,576 to
   24,531,456 bytes**, a **709,120-byte (2.81%) reduction**. The app executable grows
   by 8,704 bytes; the measured runtime comparison below accounts for both.
4. **Hidden library memory:** after 15 seconds of playback or minimization, on the
   next application pump, GPU thumbnail retention drops to at most 8 MiB of recent
   images; shadow bitmaps and queued software thumbnail pixels are released.
   In-flight generations are invalidated. The worker, device and disk cache remain
   usable on return. This bounds retained cache ownership; released references do
   not guarantee an immediate equal reduction in driver-reported memory.
5. **Progress capture:** five-second progress autosaves use a specialized immutable
   snapshot capture. Unchanged records share 128-reference blocks, and a progress
   update clones one block plus its small directory. General edits, forced saves,
   close and reload still use full comparison, including directly edited public
   records. Root, membership, catalog/playlist revisions and target identity guard
   the fast path. On-disk JSON/journal formats are unchanged.
6. **Playback pacing:** the main player uses the audio clock instead of
   `display-vdrop`. Direct D3D11VA, zero extra requested decoder frames and the
   existing read-ahead limits remain. The measured resource reduction was a
   better tradeoff than changing renderers or forcing one decoder thread.

## Progress-capture measurements

Both source versions are compiled before measurements. The order is before,
after, after, before, with one warmup plus seven measured samples per workload in
each process. The path index is built outside capture timing in both versions,
matching production startup. This corrects an exploratory run that accidentally
charged index construction only to the new path; that run is excluded. C++ heap
counts are requested allocation bytes, not process RSS. Serialization/hash
verification is outside the timed capture. All before/after output hashes match.

| Workload | Before median | After median | Allocated bytes before → after |
|---|---:|---:|---:|
| 500 records, unchanged progress | 0.0495 ms | 0.0060 ms | 168 → 288 |
| 500 records, one changed record | 0.0470 ms | 0.0085 ms | 10,599 → 4,832 |
| 25,000 records, unchanged progress | 7.5830 ms | 0.0095 ms | 168 → 288 |
| 25,000 records, one changed record | 8.8555 ms | 0.0215 ms | 402,599 → 7,904 |

The large changed-record capture is **99.76% faster** and allocates **98.04% fewer
bytes**. This is the UI-thread capture step, not total durable-save time. The
unchanged path uses 120 additional transient bytes; the new block directory also
has small steady-state overhead. [Raw samples and summary](../evidence/backend-balance-20261017/capture-summary.json).

## Complete application measurements

The same 1080p30 H.264 clip, fresh portable profiles and 1280×800 outer windows
were used for three measured launches after one warmup. Samples start after ten
seconds of settling. Playback advancement was verified through each player's
owned-process control interface. These use warm OS caches and default decoder /
renderer settings, not identical feature or picture-quality configurations.
The Aperture baseline block ran before the candidate/competitor block; competitor
order rotated within the latter. CPU is derived from the first-to-last sample
delta; memory is the median within each run and then across runs.

| Runtime | Visible window | Private RAM | Dedicated GPU accounting | CPU, one core | Runtime files |
|---|---:|---:|---:|---:|---:|
| Aperture before | 123.89 ms | 256.58 MiB | 173.26 MiB | 38.58% | 28.52 MiB |
| Aperture after | 141.59 ms | 251.31 MiB | 169.49 MiB | 22.14% | 27.85 MiB |
| VLC | 1,952.08 ms | 144.73 MiB | 117.90 MiB | 3.68% | 182.99 MiB |
| MPC-HC | 792.92 ms | 141.81 MiB | 101.87 MiB | 3.35% | 72.02 MiB |

The complete app reduced sampled CPU by **42.6%**, private RAM by **5,525,504
bytes**, GPU accounting by **3,952,640 bytes**, and runtime files by **700,416
bytes (2.34%)**. Visible-window latency was 17.71 ms slower in this comparison.
The separate five-sample empty-profile startup gate showed the first GPU shell at
**58.20 → 57.15 ms**, populated-library frame at **297.46 → 295.00 ms**, and
private RAM **61,952,000 → 62,316,544 bytes**. Both versions passed the startup
budgets. Taken together, these do **not** establish a general startup speedup;
the large win is less work during playback and progress saves. The shell,
visible-window and engine-ready measurements have different endpoints.

Aperture wins these measured window-opening and runtime-size comparisons but
still consumes substantially more playback RAM, GPU memory and CPU than VLC or
MPC-HC. This pass does not claim the user's eventual all-three-headlines target
has been reached. Resource/quality defaults, control-interface overhead and root
process accounting limit the comparison; this is one SDR clip, not a universal
ranking. [Application samples and startup gates](../evidence/backend-balance-20261017/application-summary.json).

## Engine resource experiments

Each variant runs in a fresh isolated engine with a 1280×800 window. One warmup is
discarded, followed by three measured rounds with rotating order. The probe waits
five seconds after PLAYBACK_RESTART, then samples three seconds of playback.
CPU is percent of one logical core. Private RAM is Windows private usage; GPU
local memory is DXGI process accounting. Do not add GPU memory to RAM. Timing ends
at an engine event, **not compositor presentation or first visible video pixels**.
All pacing samples used direct D3D11VA and had zero steady-state dropped frames.

| 1080p30 H.264, audio disabled | Engine ready | Private RAM | GPU local | CPU |
|---|---:|---:|---:|---:|
| display-vdrop, automatic threads | 391.529 ms | 227,504,128 B | 149,155,840 B | 16.622% |
| audio clock, automatic threads | 366.170 ms | 219,471,872 B | 145,190,912 B | 8.828% |
| display-vdrop, one decoder thread | 366.149 ms | 230,371,328 B | 149,123,072 B | 15.069% |

The audio clock reduced sampled CPU by 46.9%, private RAM by 8.03 MB and GPU local
usage by 3.96 MB, with 6.5% faster engine readiness in this fixture.

| 720p H.264/AAC, audio processing enabled and muted | Engine ready | Private RAM | GPU local | CPU |
|---|---:|---:|---:|---:|
| display-vdrop, automatic threads | 577.494 ms | 187,514,880 B | 84,533,248 B | 17.159% |
| audio clock, automatic threads | 598.714 ms | 176,881,664 B | 80,601,088 B | 5.204% |
| display-vdrop, one decoder thread | 585.163 ms | 187,273,216 B | 84,533,248 B | 18.227% |

With audio, CPU fell 69.7%, private RAM 10.63 MB and GPU local usage 3.93 MB.
Readiness was 21.22 ms (3.7%) slower. This is an explicit speed/resource tradeoff,
not an across-the-board startup improvement. Longer playback, other refresh
rates, video quality and AV synchronization still require wider workloads.
[1080p samples](../evidence/backend-balance-20261017/pacing-comparison.json),
[audio-enabled samples](../evidence/backend-balance-20261017/pacing-audio.json).

## Experiments not adopted

**Renderer switch:** on the 720p fixture with the new DLL, `gpu-next` increased
median readiness from 406.172 to 423.252 ms, private RAM from 183,123,968 to
185,925,632 bytes, GPU local usage from 84,533,248 to 86,458,368 bytes, and CPU
from 17.563% to 18.607%. Keep `gpu`. This comparison does not match all renderer
quality defaults or establish an HDR verdict. The smaller DLL alone did not show
a consistent startup speed win. [Renderer samples](../evidence/backend-balance-20261017/renderer-comparison.json).

**Decoder surfaces:** inspected the pinned FFmpeg D3D11/DXVA allocation path,
mpv's extra-frame adjustment and FFmpeg's additional threading/output surfaces.
`hwdec-extra-frames=0` does not mean a zero or one-surface decoder. H.264/HEVC
reference-frame requirements remain, and actual final pool size depends on these
other paths. The one-thread control did not reduce GPU usage materially and
increased private RAM on the 1080p fixture. No speculative pool reduction or
thread cap is shipped. The resource probe exposes a repeatable way to evaluate
future decoder changes; it does not count live decoder surfaces directly.

**Indexed/paged store:** the isolated Python prototype verifies equal records
for full hydration and the first 50 title-sorted records. JSON full hydration was
100.52 ms versus SQLite's 179.96 ms. JSON occupied 7,894,171 bytes versus
10,391,552 bytes for SQLite. Retrieving only the first 50 rows was 116.35 ms for
JSON versus 1.81 ms for indexed SQLite, with 38.65 MB versus 87 KB of Python heap.
These are **Python container measurements, not native Aperture measurements**;
SQLite C allocations, production collation/filter semantics, migration and the
native model are excluded. Paging can help only when consumers stop requiring
the entire catalog. A file-format-only swap worsened the measured balance, so
the prototype remains an experiment and no production database is added.
[Store experiment](../evidence/backend-balance-20261017/paged-store.json).

These dispositions cover all eight research recommendations: critical path,
saved sort, engine dead code, decoder attribution, hidden cache lifecycle,
incremental progress capture, renderer A/B, and paged storage evaluation.

## Validation and portability

- Local Release build and **22/22 PR suites passed**, including **51 unit tests**.
  The five added cases test immutable block boundaries, real progress write/reload,
  saved query ordering/reuse, import intent/cancellation, and real D2D thumbnail
  release/re-admission. Existing persistence, loading and shutdown checks also pass.
- Rebuilt DLL: **59 functional playback checks passed**, including hardware and
  software formats, seeking, subtitles, HDR decoding, screenshots, software
  rendering, audio filters and HTTP playback. This does not establish visual HDR
  or Dolby Vision quality. [Playback log](../evidence/backend-balance-20261017/engine-functional.log).
- The real-application workflow also passed playback, seek/pause, queue changes,
  three save/restart cycles, volume persistence, and private-session persistence
  checks. Its **226 commands had 38.36 ms p95 latency**. An initial failure was
  an outdated test reader expecting a bare array: the retained file was a valid
  schema-2 object with the correct resume position. The reader now requires the
  current schema and inspects its records, including private-mode assertions.
  [Final workflow log](../evidence/backend-balance-20261017/app-workflows.log).
- Runtime storage remains executable-local. No production database, external
  profile path, extra service, shader cache location or new runtime dependency is
  introduced. Benchmarks use isolated development scratch and redirected child
  profiles. Existing portable-store and package-boundary gates passed; this is
  not a complete OS-level filesystem trace of driver activity.
- Local release/debug copies were installed with saved data retained. Version
  remains 2.0.0.0; no release tag or GitHub asset was published. Both installed
  executables and DLLs match the measured candidate hashes. Temporary engine-build
  and probe copies remain in development scratch: automatic approval review
  rejected their removal with a policy restriction. They are excluded from the
  runtime package and its footprint.

Engine provenance: [locked sources/toolchain/hash](../evidence/backend-balance-20261017/engine-provenance.json)
and [package inventory](../evidence/backend-balance-20261017/engine-packages.txt).
The new DLL SHA-256 is
`11b37889e5103a1370509dcf5e5a9c389325c497552f522f45049dfba19f5c52`.

## Primary references

- [SPIRV-Cross C API backend selection](https://github.com/KhronosGroup/SPIRV-Cross/blob/vulkan-sdk-1.4.357.0/spirv_cross_c.cpp)
  and [build options](https://github.com/KhronosGroup/SPIRV-Cross/blob/vulkan-sdk-1.4.357.0/CMakeLists.txt).
- [libplacebo D3D11 shader path](https://github.com/haasn/libplacebo/blob/v7.360.1/src/d3d11/gpu_pass.c).
- [Pinned FFmpeg decoder surfaces](https://github.com/FFmpeg/FFmpeg/blob/140fd653aed8cad774f991ba083e2d01e86420c7/libavcodec/dxva2.c)
  (the exact source revision is authoritative in the provenance file).
- [mpv decoder pool adjustment](https://github.com/mpv-player/mpv/blob/v0.41.0/video/decode/vd_lavc.c)
  and [video output documentation](https://mpv.io/manual/stable/#video-output-drivers).
- [DXGI process memory reporting](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo).
- [SQLite application containers](https://www.sqlite.org/appfileformat.html)
  and [temporary files](https://www.sqlite.org/tempfiles.html).
