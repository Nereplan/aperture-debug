# Testing

Run checks locally from the repository root on Windows x64 with the build prerequisites in [Build and release](BUILD_AND_RELEASE.md). [tests/suites.json](../tests/suites.json) is the authoritative suite inventory; the runner validates paths and declarations before executing suites with timeouts and cleanup.

## Select checks

```powershell
./tests/run_tests.ps1 -ValidateOnly
./tests/run_tests.ps1 -List
./tests/run_tests.ps1 -Tier pr -FailFast
```

The default selection is the PR tier. It currently contains 23 suites covering library workflows, units, malformed inputs, persistence, IPC, loading, diagnostics, updates, package/install boundaries, static analysis, shutdown stress, media-probe isolation, thumbnails, and rival benchmark contracts. These names identify local groups; no GitHub Actions runner is used.

Select suites by inventory ID when working on a specific boundary:

```powershell
./tests/run_tests.ps1 -Id unit,persistence -FailFast
./tests/unit/run_tests.ps1 -Suite GlobalSearch
./tests/unit/run_tests.ps1 -Suite DuplicateDetection
./tests/workflows/run_tests.ps1 -Seed 12345
```

The native unit/backend/loading/persistence/IPC/loader/updater/stress harnesses register 218 behavioral contract tests (`165` unit, `9` backend, `8` loading, `28` persistence, `3` IPC, `3` updater, `1` loader, `1` stress) and reject an unknown selection, an empty selection, and a test that executes no assertions. Explicit skips are reported separately. Queue/session persistence runs independently of the opt-in visible-window geometry test. The central invocation shares production objects across the workflow, unit, corpus, and persistence suites in a temporary content-keyed pool; harness sources compile fresh. Running these scripts independently does not share that pool.

The [backend behavior suite](../tests/backend/README.md) exercises actual portable stores, cache operations, numeric formatting and allocation bounds. It replaces simulated persistence tests with real round trips. Run `./tests/backend/verify_mutations.ps1` to verify that deliberate production defects are rejected by their specific assertions.

The [portable loading pipeline suite](../tests/loading/README.md) adds eight independent
regressions for streaming, real restart/relocation, malformed suffix recovery,
journal replay, schema write protection and comparative heap budgets. Run
`./tests/run_tests.ps1 -Id loading` and `./tests/loading/verify_mutations.ps1`.
The save-worker scenarios are individually selectable under `LibrarySaving`.
`BackendBalance` checks immutable snapshot blocks, persisted progress capture,
saved startup ordering, and cancellation/new-intent fencing for imports.
`RendererCache` also checks real bitmap eviction and pending pixel cleanup when
the library is hidden, followed by re-admission on return.

## Choose the appropriate scope

| Tier or suite | Purpose and prerequisites |
|---|---|
| `pr` | Routine regression gate; MSVC, Windows SDK, Python, and bundled dependencies |
| `package` | Package verification, PE dependencies, signing, and symbol archive checks; inspect each suite's artifact requirements with `-List` |
| `fuzz` | Bounded malformed-input campaigns; toolchain and sanitizer requirements depend on the selected script |
| `manual` | Desktop/render/audio checks, playback, benchmarks, mutation checks, verifier, and reproducibility; inspect prerequisites and pass explicit fixture paths as needed |
| `app_workflows` | Real isolated executable and generated PCM media; defaults to `release/Aperture.exe` and refuses to interact with an existing app session |
| `workflow_mutations` | Two deliberately broken source copies must fail the expected behavioral assertions; compilation failures do not count |

Use [workflow testing](../tests/workflows/README.md) for playback/restart coverage and seed replay. Use [thumbnail testing](../tests/thumbnails/README.md) for codec, migration, cache accounting, and isolated corpus experiments. Run suites sequentially when they write the same diagnostic paths. `-Tier all` includes checks that need special tooling, fixtures, or desktop access.

`./build_and_release.ps1 -FailFast` builds Release, runs the PR gate, packages, and installs the local copies while retaining saved data. It does not publish to GitHub. Use that command when preparing an app update; use the focused checks while iterating.

## Performance and visual evidence

Use the benchmark drivers and matched-baseline instructions in [Measured performance improvements](reports/PERFORMANCE_OPTIMIZATION.md), [Backend optimization](reports/BACKEND_OPTIMIZATION_REPORT.md), and [Test efficiency and disk usage](reports/TEST_AND_STORAGE_OPTIMIZATION.md). Keep temporary baseline checkouts, binaries, and large logs outside the project. Record matching result hashes as well as timing; avoid concurrent compilation during comparisons. CPU/heap microbenchmarks do not establish playback frame rate or video quality.

The [initial optimization report](reports/OPTIMIZATION_REPORT.md) records a known mismatch between the checked-in visual goldens and its original baseline, plus a separate passing baseline-versus-optimized pixel comparison. A PR-tier pass does not establish that the manual visual suite or every environment-dependent check passes. Review a golden update against an independently accepted UI before changing expected images.
