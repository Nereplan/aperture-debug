# Performance checkpoints

Open [checkpoints.html](checkpoints.html) in a browser. This is the primary performance record and replaces the report documents as the everyday comparison and update workflow. It is one self-contained HTML file: no server, network, external assets or Python is needed to view it. Copying the HTML alone preserves its graphs, measurements, instructions, validation and historical report text.

The dashboard opens on chronological performance history. **Timeline** connects every selected recorded Aperture observation for a metric in measurement-date order; the date and commit identify each checkpoint. **Resource map** connects playback snapshots in the same order, with arrows showing progression. Checkboxes control inclusion. The history metric selector covers playback and startup; use **Graph** in **Benchmarks** for a particular internal workload. **Evidence** preserves source identity, recorded test results, protocols and reports. Export buttons download copies without editing the HTML.

VLC is one fixed reference from the latest recorded version, using the newest available measurement for the selected metric and matching protocol/workload/environment. Older VLC measurements remain in raw evidence and reports; they are not plotted as a VLC history. The reference label/tooltip records its version and measurement date. Historical Aperture observations are intentionally compared with this current reference, not necessarily with a same-session VLC run.

**Cuts** is the default numeric scale. Large intervals containing no observations are omitted and marked with axis breaks. Within retained bands the unit scale stays linear; values are never rounded, moved or invented to fit. Resource-map cuts apply to both numeric axes; Timeline cuts apply to the value axis. Linear and logarithmic scales remain available. Timeline stops follow date order with categorical checkpoint spacing; spacing does not claim elapsed duration or invent exact times for date-only records.

Recorded CPU measurements and CPU history use percent of **one logical processor**. The summary card shows **total machine capacity** when the environment has a verified `logicalProcessors` count, and shows its VLC difference in percentage points. For this 24-processor machine, 22.1392% / 24 = 0.9225% total capacity; VLC's 3.6835% / 24 = 0.1535%. The +501% relative ratio means 6.01 times that small VLC value, a +0.7690 percentage-point difference. This normalization is approximately comparable with a whole-machine CPU display; frequency-adjusted utilization counters and different observation windows can differ.

Hover chart points for measured run ranges, where retained. **Evidence → Measurement review** explains the historical playback outliers and a separate 45-launch startup replay. It did not reproduce the original ~30 ms slowdown consistently. The legacy three-launch, 20 ms polling sessions are observed history, not proof that a commit caused a regression. The replay uses a different observer and stays out of the original series. See `docs/evidence/checkpoint-audit-20261017/measurement-review.json` and `startup-replay.json`; `tests/benchmark/audit_player_measurements.py` recomputes the audit from raw counters. To repeat the saved-runtime startup check, pass its `inputs` object as a manifest to `tests/benchmark/audit_saved_startup.ps1`, with a new external scratch `-OutputDir`. It verifies original EXE/DLL hashes, preserves source runtimes, rotates launch order and records actual observation bounds. The 14 October DLL lacks an independently saved historical hash; its retained-runtime hash is explicit.

The legacy VLC comparison is one synthetic **1080p30 H.264 clip without audio/subtitles**, with equal outer-window dimensions, fresh profiles and verified advancing playback. It does not establish equal decoding, viewport, presentation cadence or picture quality. Aperture shows its bootstrap shell before playback initialization, so visible-window time is not equivalent video readiness. Original CPU sampling spans only about six seconds per run and covers different media positions (final Aperture approximately 10.6–16.77 s, VLC 7–13 s); positions were not aligned and presented frames were not verified equal. Memory and GPU figures overlap and cannot be summed. Complete distribution sizes are not feature-matched efficiency. These limits belong in the protocol and review; do not infer a general player ranking or a component-level cause from the ratios.

## Add a checkpoint

From the repository root, using Python 3 and PowerShell:

```powershell
$checkpointScratch = Join-Path $env:LOCALAPPDATA 'Codex/tools/scratch/aperture-checkpoints'
New-Item -ItemType Directory -Force -Path $checkpointScratch | Out-Null
python scripts/checkpoints.py --template "$checkpointScratch/next.json"
# Fill next.json with the measured source state, observations and validation.
python scripts/checkpoints.py --append "$checkpointScratch/next.json"
python scripts/checkpoints.py --check
python tests/benchmark/test_checkpoints.py
# When changing chart logic (requires Node.js):
node tests/benchmark/test_checkpoint_charts.cjs
git diff --check
```

`--list` prints checkpoint IDs. `--export <new-file.json>` exports the full embedded dataset and its definitions. `--html <path>` selects a different dashboard copy. Export/template commands refuse to overwrite an existing output file. The append command validates the complete proposed dataset before atomically replacing the HTML, rejects duplicate checkpoints/results, and cannot redefine historical metric/protocol/environment/reference definitions. Keep meaningful changes in a descriptive Git commit.

Each bundle contains a `checkpoints` array. Add new definitions in `metrics`, `protocols`, `environments`, `references`, or `historySeries` only when needed. The generated template shows the result and validation fields. A complete checkpoint must include:

- A stable ID, descriptive label, measurement date/time, source commit, dirty flag and source/build identity note. Exact timestamps require a timezone; use a date alone when the original time is unavailable. Record hashes in the source note or evidence. Never present a modified build as a clean commit.
- Results with metric ID, protocol ID, exact workload, environment, session, value, measured sample count and evidence path. Use the existing metric's units. `null` values require `missingReason`; unknown sample counts require `samplingNote`.
- Ordinary updates append the measured checkpoint to its existing history. Optional `pairedWith` metadata may preserve an original controlled experiment; it does not define the main comparison view. Keep metric/protocol/workload/environment identities accurate.
- Test names, original passed/total counts, status, durations in seconds, evidence and scope. Preserve failures and partial runs. Do not add overlapping unit-test and suite counts together.
- Notes explaining the change, experimental limits and tradeoffs. Future performance updates belong here rather than in another report document.

For a new VLC measurement, include a `references` entry with `player`, `version`, `measuredAt`, and `results`. The chart automatically uses the latest recorded VLC version as its one fixed reference, never a separate VLC time series. If a same-session measurement exists, its ID may also be saved in the Aperture result's `reference` field; that provenance link must match metric, protocol, environment, workload **and session**. Never relabel an old run as freshly measured. Internal benchmarks without a VLC equivalent have no fabricated reference.

Strict history series normally reuse the same metric, protocol, workload and environment. To join documented historical readings across protocol records, declare `historySeries` metadata with `label`, `method`, exact `workload`, and an allowed `protocols` list; set each participating result's `historySeries` to that ID. The helper rejects undeclared groups or incompatible protocols/workloads. Metrics remain separate within a group. Explain changes of sampling or machine identity in the group's method and per-point provenance. An observation can carry its own ISO `measuredAt` when it differs from the checkpoint date.

New metric definitions specify `label`, `unit` (`ms`, `MiB`, `bytes`, `%`, `count`) and `direction` (`lower`, `higher`, `neutral`). New protocols specify `label`, `category` (Playback, Startup, Storage, Library, Rendering, Subtitles, Maintenance) and `method`. New environments specify `label` and `details`. Record timing endpoints, fixture/hash, compiler/flags, warmups, sample count, order, cache state, affinity, verification, aggregation and accounting bounds in these definitions and evidence. The embedded guide explains delta calculations and units.

Record a verified positive integer `logicalProcessors` on an environment to enable CPU capacity normalization. Optional result `runRange` metadata contains `min`, `max`, `runs` and `scope`, in the original metric's units; retain the aggregation (for example, a range of per-run memory medians). Never substitute a range or a retrospective replay for the original value. A checkpoint's optional `measurementReview` holds retrospective interpretation with its evidence path. Preserve the source measurements and distinguish unknown causes from measured differences.

## Reproduction and migration bounds

Use the existing [benchmark runners](../tests/benchmark/README.md), [backend harness](../tests/backend/README.md), [loading tests](../tests/loading/README.md) and [test guide](TESTING.md). The dashboard displays saved measurements; it does not execute the media player or benchmark runners. No app rebuild is required for dashboard changes. All actual benchmarks/builds remain local under the repository release policy.

The initial migration normalizes explicit paired JSON summaries from the query, duplicate, removal, subtitle, UI refresh, backend, JSON ownership, library-record, thumbnail-removal, loading and playback/capture studies. Forward/reverse loading trials stay separate. Complete-app and empty-profile startup endpoints stay separate. Archived reports and runner guides are embedded in full; experiments without normalized pairs remain available in that library rather than being inferred into numerical series. Raw CSVs, hashes and logs remain in `docs/evidence/` as optional companions, while their paths are recorded inside the HTML.

Historical commits, exact times and hardware identities were sometimes incomplete. Unknowns remain explicit. Study-specific protocols and environments prevent unrelated harnesses from being joined automatically. The declared empty-profile startup history adds seven dated readings from 3–17 October: the first is a single captured sample, while later gates use five-run medians. These are observed history, not a claim of a controlled causal speedup. The legacy silent-playback series retains its three matching Aperture snapshots; earlier playback values are not invented. A separate 17 October audio-enabled replay records the preserved pre-fix build and the optimized build across one warmup and three measured rounds per player, using ~30-second CPU windows over corresponding media segments. See `docs/evidence/direct-playback-20261017/optimization-review.json`. Selecting one of these checkpoints switches the resource map and cards to its matching workload/protocol; it does not merge audio and silent sessions. The replay checkpoints share the batch completion timestamp because the runs were interleaved; exact per-run times and ranges remain in the evidence. The local 22-suite gate, 59 functional playback checks and live overlay/return-to-library checks passed. This validates a substantial improvement, not an overall performance win over VLC or decoder/picture-quality equivalence between players. Raw 14 and 17 October VLC records remain intact, while the UI uses one latest-version reference. Percentages across studies are never compounded.


New player comparisons can match the actual video canvas with `compare_players.ps1
-VideoWidth 1280 -VideoHeight 720 -ForceD3D11Linear`. Keep the returned child-window
sizes/DPI and rendering controls. Outer windows may differ because player chrome
occupies different areas. The recorded `kLinear` comparison profile in
`src/PlaybackScaling.h` uses bilinear scaling throughout. It is a performance
profile with a picture-quality tradeoff, especially on fine detail and larger
scale changes. The app's balanced candidate retains Lanczos upscaling, Hermite
downscaling and correct downsampling, uses bilinear chroma, and disables the
extra linear-light/sigmoid passes. Do not present the earlier all-linear results
as measurements of the balanced candidate. The full native filters remain available through custom
MPV settings (`scale=lanczos`, `dscale=hermite`, `cscale=lanczos`,
`linear-downscaling=yes`, `correct-downscaling=yes`, `sigmoid-upscaling=yes`).
Different filter profiles belong in separate protocols/workloads; matched options alone
do not establish identical decoder/output pixels.

Preserve both sampled `cpuSeconds` and raw `cpuCycles`. Periodic short work can
alias with Windows' coarse CPU-time accounting. For a hardware-specific estimate,
build/run `tests/benchmark/calibrate_cpu_cycles.cpp` outside timing trials, retain
its JSON/source hash and require its invariant-TSC, stable-rate and busy-counter
validation to pass on the same CPU/boot. Use `cycleDelta / elapsedSeconds /
tscTicksPerSecond * 100` only in an explicitly calibrated protocol. An unsupported
calibration is not a zero CPU measurement. Never substitute generic CPU MHz,
convert unvalidated counters, or overwrite the historical sampled-CPU values.
See `docs/evidence/matched-linear-20261018/review.json` for the first such batch.

Validation-only checkpoints can use `results: []` with explicit test scopes and
notes when no new performance measurement was taken. The dashboard keeps the
latest measured observation selected and opens these records in Evidence.
Retain desktop skips as partial validation; do not replace missing visible
measurements with hidden-window performance or relabel saved VLC runs as fresh.
