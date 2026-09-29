# Workflow testing

These suites complement focused unit tests with user-visible outcomes. They
call production code or the real executable; they do not simulate save/load
by copying its implementation into a test.

## Library and portable-profile scenarios (PR gate)

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/workflows/run_tests.ps1
```

Each run uses three regression seeds and a fresh random seed, printed before
execution and saved in `diagnostics/workflows/latest-library.json`. Replay any
failure with `-Seed 12345`, or supply a comma-separated seed list.

Each seed covers empty, single-item, small, medium and 25,000-item libraries
with shuffled ordering, Unicode titles, varied durations and playback history.
An independent numeric oracle checks ordering and membership across ascending,
descending, capped and unlimited queries and smart playlists. User title edits
must appear immediately in an already-warm search index. Grouping must neither
lose nor duplicate members.

Disk workflows generate real WAV files, import them twice, create a playlist,
replay 60 progress changes with periodic saves/reopens, remove an item, save
preferences and bookmarks, and move a complete portable profile. Assertions
check resulting memberships, resume positions, Unicode data, unknown-field
preservation, and relocated media paths through actual production APIs.

## Actual executable and playback engine

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/workflows/run_tests.ps1 -AppOnly
```

Uses `release/Aperture.exe` by default; `-Executable` selects another build.
The suite stages a separate runtime and generates silent PCM fixtures, so it
requires no personal media, downloads or additional codecs. Through the public
automation protocol it verifies decoded durations, seek accuracy, an advancing
playback clock, a stationary paused clock, queue next/previous, stop, clean
shutdown, and correct saved/reloaded resume positions over three launches.
It records command latency and enforces a broad 1-second p95 responsiveness
budget, with per-command and per-transition timeouts. Each request requires a
valid response or acknowledgement; process liveness alone cannot pass a step.

An existing Aperture session causes a failure before interaction, so the suite
cannot silently drive the user's session. The runner targets only its owned
process/window and cleans up on failure. It belongs to the manual tier because
it requires Windows desktop/render/audio facilities. It is also available via
`tests/run_tests.ps1 -Id app_workflows`.

This covers actual audio decoding, playback control, startup and persistence.
It does **not** establish video quality, GPU throughput, pointer hit-testing,
accessibility usability or visual smoothness. Rendering/visual suites and
interactive testing remain necessary for those concerns. The old pointer smoke
test now requires an explicit `-VideoPath` and copies a personal profile only
when `-DataSource` is explicitly provided.

## Check the tests themselves

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/workflows/verify_mutations.ps1
```

This builds isolated source copies with two deliberate regressions: reversed
duration ordering and zeroed saved resume positions. Both must fail the
specific behavioral assertion. A build error, unrelated failure, or successful
test does not count as detecting the mutation. Production source is untouched.

## Artifacts and cleanup

All generated media, builds and profiles live in unique temporary directories.
The default retains only the latest compact log and JSON result for each mode
under `diagnostics/workflows`. Use `-KeepArtifacts` when investigating a failed
run; the report records its location. Every reported random seed is replayable.
Reports from mutation checks are kept under fixed names to avoid unbounded
artifact growth. A normal production-source run refreshes `latest-library.*`
after mutation verification.
