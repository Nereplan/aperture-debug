# Performance checkpoints

Open [checkpoints.html](checkpoints.html) in a browser. This is the primary performance record and replaces the report documents as the everyday comparison and update workflow. It is one self-contained HTML file: no server, network, external assets or Python is needed to view it. Copying the HTML alone preserves its graphs, measurements, instructions, validation and historical report text.

The dashboard opens on chronological performance history. **Timeline** connects every selected recorded Aperture observation for a metric in measurement-date order; the date and commit identify each checkpoint. **Resource map** connects playback snapshots in the same order, with arrows showing progression. Checkboxes control inclusion. The history metric selector covers playback and startup; use **Graph** in **Benchmarks** for a particular internal workload. **Evidence** preserves source identity, recorded test results, protocols and reports. Export buttons download copies without editing the HTML.

VLC is one fixed reference from the latest recorded version, using the newest available measurement for the selected metric and matching protocol/workload/environment. Older VLC measurements remain in raw evidence and reports; they are not plotted as a VLC history. The reference label/tooltip records its version and measurement date. Historical Aperture observations are intentionally compared with this current reference, not necessarily with a same-session VLC run.

**Cuts** is the default numeric scale. Large intervals containing no observations are omitted and marked with axis breaks. Within retained bands the unit scale stays linear; values are never rounded, moved or invented to fit. Resource-map cuts apply to both numeric axes; Timeline cuts apply to the value axis. Linear and logarithmic scales remain available. Timeline stops follow date order with categorical checkpoint spacing; spacing does not claim elapsed duration or invent exact times for date-only records.

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

## Reproduction and migration bounds

Use the existing [benchmark runners](../tests/benchmark/README.md), [backend harness](../tests/backend/README.md), [loading tests](../tests/loading/README.md) and [test guide](TESTING.md). The dashboard displays saved measurements; it does not execute the media player or benchmark runners. No app rebuild is required for dashboard changes. All actual benchmarks/builds remain local under the repository release policy.

The initial migration normalizes explicit paired JSON summaries from the query, duplicate, removal, subtitle, UI refresh, backend, JSON ownership, library-record, thumbnail-removal, loading and playback/capture studies. Forward/reverse loading trials stay separate. Complete-app and empty-profile startup endpoints stay separate. Archived reports and runner guides are embedded in full; experiments without normalized pairs remain available in that library rather than being inferred into numerical series. Raw CSVs, hashes and logs remain in `docs/evidence/` as optional companions, while their paths are recorded inside the HTML.

Historical commits, exact times and hardware identities were sometimes incomplete. Unknowns remain explicit. Study-specific protocols and environments prevent unrelated harnesses from being joined automatically. The declared empty-profile startup history adds seven dated readings from 3–17 October: the first is a single captured sample, while later gates use five-run medians. These are observed history, not a claim of a controlled causal speedup. Playback currently has three matching recorded Aperture snapshots; earlier playback values are not invented. Raw 14 and 17 October VLC records remain intact, while the UI uses one latest-version reference. Percentages across studies are never compounded.
