# Performance checkpoints

Open [checkpoints.html](checkpoints.html) in a browser. This is the primary performance record and replaces the report documents as the everyday comparison and update workflow. It is one self-contained HTML file: no server, network, external assets or Python is needed to view it. Copying the HTML alone preserves its graphs, measurements, instructions, validation and historical report text.

The dashboard starts with the latest recorded complete-app checkpoint. Choose a checkpoint in the right sidebar, compare with its matched VLC session or paired Aperture baseline, and use **Graph** to inspect a particular workload. Checkboxes control plotted checkpoints. **Benchmarks** searches all normalized observations; **Evidence** shows source identity, recorded test results, measurement methods and searchable embedded reports. **Agent guide** contains the maintenance contract. Export buttons download data copies; they do not change the saved HTML.

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
git diff --check
```

`--list` prints checkpoint IDs. `--export <new-file.json>` exports the full embedded dataset and its definitions. `--html <path>` selects a different dashboard copy. Export/template commands refuse to overwrite an existing output file. The append command validates the complete proposed dataset before atomically replacing the HTML, rejects duplicate checkpoints/results, and cannot redefine historical metric/protocol/environment/reference definitions. Keep meaningful changes in a descriptive Git commit.

Each bundle contains a `checkpoints` array. Add new definitions in `metrics`, `protocols`, `environments`, or `references` only when needed. The generated template shows the result and validation fields. A complete checkpoint must include:

- A stable ID, descriptive label, measurement date/time, source commit, dirty flag and source/build identity note. Exact timestamps require a timezone; use a date alone when the original time is unavailable. Record hashes in the source note or evidence. Never present a modified build as a clean commit.
- Results with metric ID, protocol ID, exact workload, environment, session, value, measured sample count and evidence path. Use the existing metric's units. `null` values require `missingReason`; unknown sample counts require `samplingNote`.
- The ID of its paired baseline in `pairedWith`, if one exists. Keep the same metric/protocol/workload/environment identity only when the conditions truly match.
- Test names, original passed/total counts, status, durations in seconds, evidence and scope. Preserve failures and partial runs. Do not add overlapping unit-test and suite counts together.
- Notes explaining the change, experimental limits and tradeoffs. Future performance updates belong here rather than in another report document.

For a new VLC comparison, include a new `references` entry with `player`, `version`, `measuredAt`, and `results`. Assign its ID to the corresponding Aperture results' `reference` field. Every linked reference must match metric, protocol, environment, workload **and session**. A new session needs newly measured VLC values. Internal Aperture benchmarks have no invented VLC equivalent.

New metric definitions specify `label`, `unit` (`ms`, `MiB`, `bytes`, `%`, `count`) and `direction` (`lower`, `higher`, `neutral`). New protocols specify `label`, `category` (Playback, Startup, Storage, Library, Rendering, Subtitles, Maintenance) and `method`. New environments specify `label` and `details`. Record timing endpoints, fixture/hash, compiler/flags, warmups, sample count, order, cache state, affinity, verification, aggregation and accounting bounds in these definitions and evidence. The embedded guide explains delta calculations and units.

## Reproduction and migration bounds

Use the existing [benchmark runners](../tests/benchmark/README.md), [backend harness](../tests/backend/README.md), [loading tests](../tests/loading/README.md) and [test guide](TESTING.md). The dashboard displays saved measurements; it does not execute the media player or benchmark runners. No app rebuild is required for dashboard changes. All actual benchmarks/builds remain local under the repository release policy.

The initial migration normalizes explicit paired JSON summaries from the query, duplicate, removal, subtitle, UI refresh, backend, JSON ownership, library-record, thumbnail-removal, loading and playback/capture studies. Forward/reverse loading trials stay separate. Complete-app and empty-profile startup endpoints stay separate. Archived reports and runner guides are embedded in full; experiments without normalized pairs remain available in that library rather than being inferred into numerical series. Raw CSVs, hashes and logs remain in `docs/evidence/` as optional companions, while their paths are recorded inside the HTML.

Historical commits, exact times and hardware identities were sometimes incomplete. Unknowns remain explicit. Historical before/after summaries use study-specific protocols and, where identity is incomplete, study-specific environments, so the UI cannot silently join unrelated harnesses. The 14 and 17 October player comparisons retain their own freshly measured VLC references. Percentages from different studies are never compounded. Original Markdown and evidence are preserved as historical source material; future agents should start with the dashboard.
