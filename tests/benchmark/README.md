# Local player comparison

`compare_players.ps1` measures complete Windows player runtimes without installing
them or changing existing player settings. Use PowerShell 7 and clean runtime
directories. MSVC is used to compile the small Aperture status observer, unless
`-ApertureStatusProbe` supplies an existing copy.

```powershell
./tests/benchmark/compare_players.ps1 `
    -AperturePath C:/bench/aperture/Aperture.exe `
    -VlcPath 'C:/Program Files/VideoLAN/VLC/vlc.exe' `
    -MpcHcPath C:/bench/mpc-hc/mpc-hc64.exe `
    -MediaPath C:/bench/media/h264-1080p30.mp4 `
    -OutputDir "$env:LOCALAPPDATA/Codex/tools/scratch/player-comparison"
```

The defaults are one warmup and three measured rounds, with player order rotated.
Use `-PlayerNames Aperture,VLC` to restrict the selected players. Every run stages
a fresh runtime, uses executable-local portable configuration, and redirects the
child process's ordinary profile and temporary paths. App-owned runtime/config
growth is recorded before the generated copies are removed. Source media and
original installations are never changed. Existing same-name processes cause a
refusal to launch so singleton routing cannot affect a user's session.

The latency metric ends at the first visible process-owned top-level window. It
does **not** measure the first decoded video frame. Windows are set to the same
outer dimensions both before and after loading. Samples start after eight seconds
plus two seconds to settle the second resize; GPU/status queries add overhead.
Playback advancement is checked with Aperture's authenticated IPC, VLC's owned
localhost RC socket, and MPC-HC's owned `/slave` window. Failure to verify is
reported explicitly. The latter two interfaces follow the official
[VLC RC implementation](https://github.com/videolan/vlc/blob/3.0.23/modules/control/oldrc.c)
and [MPC-HC API](https://github.com/clsid2/mpc-hc/blob/2.8.3/src/mpc-hc/MpcApi.h).

`comparison.json` retains all samples, arguments, runtime hashes, versions,
footprints and limitations. GPU failures produce null values with reasons, never
invented zero usage. CPU seconds cover the player process; divide the change in
CPU seconds by the change in sample time for average logical-core use. Summarize
the median sample per run, then the median across measured runs. Do not include
warmups or failed/unverified runs in a playback resource ranking.

This is a warm-OS-cache, fresh-application-profile comparison. Decoder defaults,
video viewport dimensions, rendering quality, telemetry overhead and feature sets
can differ. Process GPU accounting is driver reporting, not independently measured
exclusive physical VRAM. Do not add it to process RAM or treat one clip as a
universal player ranking. Complete supplied runtime sizes include their different
languages, plugins, update tools and other payload; they are not matched feature
subsets or compressed download sizes.

## Media record memory

`run_media_record_benchmark.ps1 -OutputDirectory <scratch-directory>` measures
record construction, independent copying and a display-title/progress scan with
500 and 25,000 records. Optional preferences and title overrides are present on
0%, 10% or 100% of the records. Each workload discards one warmup and records seven
samples on one available logical processor; hashes check equivalent contents.
`-SourceDirectory` selects a baseline source snapshot, and `-KeepBuild` retains
the small executable for alternating before/after runs without recompilation.

`results.csv` reports requested C++ allocation bytes, including the record vector
and its owned fields. Retained bytes are measured before those records are
destroyed. Copy costs exclude the pre-existing input. Scan costs include no
allocation. This is an isolated record benchmark, excluding library lookup
indexes, allocator overhead, process RSS, playback and GPU memory. Use the save
and startup benchmarks for broader behavior, and compare dense as well as sparse
optional-field workloads when evaluating a memory/speed tradeoff.

## Playback balance and progress capture

`compare_playback_resources.ps1 -Probe <compiled-mpv_playback_test.exe>
-BaselineDll <old-dll> -CandidateDll <new-dll> -MediaDir <fixtures>
-OutputDir <external-scratch>` rotates `gpu` and `gpu-next` across fresh engines.
`-Experiment Pacing` compares display-vdrop/audio clocks and a one-thread decoder
control; `-WithAudio` enables silent audio processing. `-MediaFile` selects one
fixture. Build the probe with `tests/libmpv/run_tests.ps1 -Scenario version`.
One warmup and three measured rounds are the defaults. Initialization and
PLAYBACK_RESTART are engine milestones, not compositor presentation. CPU is
percent of one logical core; DXGI local/nonlocal memory is process accounting.
Playback advancement and direct D3D11VA decoding must pass. Counters are sampled
after settling; steady frame drops exclude startup. Child profiles stay inside
each isolated run. The driver retains its copies and raw output for review.

`run_save_benchmark.ps1 -CompileOnly -SourceDirectory <src> -OutputDirectory
<scratch>` builds a reusable capture probe. Compile both versions first, then
alternate the executables with `--capture-only --playback-capture`. The path
index is prepared before timing, matching startup publication in the app. The
baseline falls back to full capture if the specialized API is absent. Mode 4 is
unchanged progress; mode 5 changes one record. Compare hashes, allocation bytes
and timing together. These measurements exclude disk I/O and index construction.

`python tests/benchmark/paged_store_experiment.py --source <generated-array.json>
--output <fresh-scratch-directory>` evaluates full hydration and a first sorted
50-record page in JSON/SQLite. It uses an isolated fixture, preserves the source,
and verifies equal hashes. Python timings and Python heap measurements are not
native-app performance; SQLite's native allocations are excluded. The experiment
is not a production database format or migration tool.


## Extended playback measurements and diagnostics

Use `compare_players.ps1 -SamplingSeconds 40 -SamplingStartMediaSeconds 10`
for longer CPU windows over corresponding media positions. The actual start/end
CPU counters and wall times are retained. VLC reports whole-second media times;
this is approximate segment alignment, not frame synchronization. Legacy defaults
remain unchanged. Record extended/audio workloads under a separate checkpoint
protocol. `-ApertureVariantsFile <json>` accepts an array of `{ "id": "name",
"runtime": "C:/bench/variant" }` entries and rotates them alongside VLC. Build all
variants before measuring. Run warmups and repeated measured rounds without
profilers, encoders, builds or other benchmark suites running.

`profile_player.cpp` builds with MSVC and `dbghelp.lib`. It uses matching Release
PDBs for symbols and live overlay fields. `run_player_profile.ps1 -Runtime <dir>
-Media <file> -Sampler <exe> -Symbols <pdb-dir> -OutputDir <new-dir>` stages an
owned runtime and profiles it after settling. Stack mode suspends threads briefly;
its stack totals describe CPU preceding a sample, not exact attribution or player
rankings. `-StateOnly` reads PDB-located fields without suspension. Check saved
state with `python tests/benchmark/check_playback_idle.py <candidate-json>
--baseline <before-json>` to verify the direct-launch hidden-overlay regression.
`-ReturnToHubWithProbe <player_status_probe.exe>` explicitly stops only the owned
player and verifies that the previously unused library renderer presents. This
requires the observer built with `--stop` support. The default observer only
queries status.

Resource-engine experiments accept `-SampleSeconds`, `-WarmupIterations` and
`-CaptureFrames`. `-Experiment Transfers` screens copy, queue and latency options;
`HardwareThreads` compares one, two and four hardware-decoder threads;
`FreeHeapCaches` measures a one-time release of free LFH caches inside the test
engine. The latter is experimental and does not change app behavior. Rendered
captures occur after timing, paused at two seconds. Capture hashes, decoder and
presentation drop counters, and raw CPU deltas are retained. Counter availability
and absence of steady frame drops must pass. A screenshot is a quality check at
one timestamp, not verification of every presented frame. To exercise functional
playback with a chosen hardware pool, use `tests/libmpv/run_tests.ps1
-HardwareDecoderThreads 1`; software decoder threading remains independent.


`run_player_profile.ps1 -CpuOnly` records per-thread CPU without suspension.
`-Player VLC` supports the same diagnostic with a fresh VLC portable profile;
`-StateOnly` and `-ReturnToHubWithProbe` are Aperture-only checks. Both players use
1280×800 outer windows by default, with `-WindowWidth` and `-WindowHeight`
overrides. These are diagnostic counters, not an output-equivalence or ranking
protocol. The comparison and profiler share `PlayerBenchmarkWindow.cs` for
owned-PID window discovery, resizing and cleanup.

`compare_playback_resources.ps1 -Experiment TimerPolicy` isolates mpv's per-wait,
always and disabled timer-resolution policies. `Presentation` compares presentation
intervals; `EagerFlush`, `GpuTimers`, `Backbuffer` and `LazyTables` require their
explicitly marked experimental DLLs. These modes keep child environment flags
isolated. `-MemoryLayout` records post-timing heap/virtual-address diagnostics;
these overlapping categories are not additive RAM measurements. `-VoCpuSections`
requires the temporary instrumented DLL and records cumulative thread cycles,
not elapsed-time estimates or player rankings.

Resource runs now retain effective `RENDER_OPTIONS`, including scale, dscale,
cscale and linear/correct downscaling. An empty cscale value is the valid inherit
default, not a failed query. `--options-only` on the native resource probe queries
configuration without loading media or collecting timing samples. `-Experiment
Scaling` compares current defaults with explicitly bilinear luma/chroma scaling
and disabled linear/correct downscaling and sigmoid upscaling. Keep those profiles
in separate protocols; identical outer windows and default settings do not
establish equal image processing or output quality between players.


`WaitRounding` requires the marked guarded MPV build and compares legacy floor,
ceiling and a one-millisecond minimum for positive waits. The full-player variant
JSON can request the same experimental policy through `environment` with
`APERTURE_TEST_ROUND_WAITS` set to `floor`, `ceil` or `min1`. It also accepts
`APERTURE_TEST_CACHE_BACKBUFFER`, `APERTURE_TEST_NO_GPU_TIMERS` and
`APERTURE_TEST_REUSE_VERTICES`, each explicitly set to `1`. Every requested flag
must have a matching marker in the DLL. Unrecognized flags/values are rejected;
inherited experimental flags are cleared for both players.
Both engine and full-player samples retain raw process cycle counters alongside
CPU time. Raw cycles have a processor-specific time base. A CPU-time estimate
requires `calibrate_cpu_cycles.cpp` to verify invariant TSC, rate stability and
the busy thread/process counters on the same CPU/boot. Retain the calibration,
label the result as an estimate and use a separate protocol; generic MHz values
are insufficient. Sampled CPU time can alias with periodic work, even in longer
windows, and short samples can quantize to zero. `wait_rounding_probe.c` is an isolated
Windows deadline-loop diagnostic (GCC, `-O2 -static -lntdll`), not a player ranking.

For matched rendering areas, `compare_players.ps1` and `run_player_profile.ps1`
accept `-VideoWidth 1280 -VideoHeight 720`; the returned child geometry and DPI
must match. `-ForceD3D11Linear` also fixes VLC's output, hardware decoder and
sampler. Equal outer windows do not imply equal video areas. CPU-only thread
profiles retain raw cycles without suspending; stack profiles retain module RVAs
for resolving the exact unstripped native binary and are diagnostic only.

Native screening modes `LinearRenderers` and `RoundedTimerPolicy` keep bilinear
scaling and positive-wait rounding fixed while varying the renderer or timer
policy. `VideoViews`, `DrawOverhead` and `DrawSubmission` require marked guarded
builds; cache counters must prove that a requested cache executed. Screen results
need repeated full-player verification before adoption. A preserved frame capture
and zero reported drops are useful checks, not exhaustive visual/pacing proof.

After applying the maintained MPV source patches, compile
`test_mpv_windows_wait.c` with GCC (`-O2 -static -std=gnu11`) and include paths for
the MPV source, generated MPV build and prefix headers. It calls the real patched
timeout conversion with an intercepted Windows sleep boundary, verifying polling,
positive remainders, finite/infinite limits, return codes and timer cleanup. It
fails at the positive-remainder case against the original pinned header. The
functional playback gate separately tests real signaling and presentation.

`QualityScaling` compares full native, balanced and all-linear profiles with the
same guarded frame-resource improvements. It retains effective options, captures
and raw resource counters. A multi-pass scaler may legitimately have zero vertex
cache hits; uploads/counter presence verify instrumentation while linear screens
still require actual reuse. Keep these picture-processing workloads separate in
the dashboard. The balanced candidate retains native Lanczos/Hermite luma scaling
and downsampling antialiasing; it removes extra linear-light/sigmoid passes and
uses bilinear chroma. It is a compromise, not an identical-quality claim.

`run_tests.ps1 -Scenario geometry` under `tests/libmpv` exercises real paused and
playing resizes, portrait geometry, rotation and H264-to-HDR-HEVC reconfiguration.
The same checks run in the full functional gate. PNG captures remain in each
owned run directory. Compare fixed paused captures across candidate/original;
moving or unaligned frames must not be claimed pixel-identical.

`VertexReuse` screens zero, one and four cached geometry entries with the balanced
scaler, preserving counters and fixed-frame captures. The guarded tiny-buffer
mode is for correctness checks only: verify actual growth/discard counters and
compare paused resize/rotation captures with the original engine. Never time
that deliberately constrained buffer.

The maintained frame-resource patch caches the swap-chain view until resize,
keeps up to four small immutable vertex ranges, and exposes
`d3d11-gpu-timing=yes|no` and `d3d11-vertex-cache-entries=0..4`. Native defaults
retain GPU diagnostics and enable four entries; Aperture disables per-pass GPU
timestamps for ordinary playback. These timestamps measure renderer cost; they
do not pace playback or change pixels. Re-enable them in custom MPV settings for
GPU profiling. The native runner accepts `-GpuTiming` and `-VertexCacheEntries`;
resource probes accept `--gpu-timing` and `--vertex-cache-entries`, retaining the
effective options when supported. Production builds use no experimental
environment switches. Verify AV1 hardware decode and explicit software playback
after rebuilding: enabling decoder flags alone is not playback evidence.
