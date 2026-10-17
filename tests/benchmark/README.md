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
