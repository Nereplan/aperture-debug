# Backend regression suite

Run `./tests/run_tests.ps1 -Id backend` or `./tests/backend/run_tests.ps1`.
This suite is part of the local PR gate. It builds production code and tests:

- Fresh/reset settings budgets, explicit user budgets and extensions through actual save/load calls.
- Library save, restart and directory relocation with Unicode paths, preferences, bookmarks, overrides, playlist and history extensions. Invalid preferences and absent fields use the production loader/writer.
- History ordering and the 100-record limit with 3,000 mixed duplicate/invalid records.
- Independent store loading after library, playlist or history corruption, including the default Favorites fallback.
- JSON numeric output against an independent classic-locale stream oracle (seed 20261015), numeric boundaries, a comma locale and a zero-allocation formatting contract.
- Real cache scans, nested classes, scan limits, replacement, locked files, byte ceilings and rejected traversal. Windows junction fixtures verify scanning/cleanup do not follow links.

The executable requires an isolation marker from the runner. All its files live
beside that executable; the temporary installation is removed on completion.
`-KeepArtifacts` retains it for investigation. Unknown test selections fail.

These scenarios replace the old tests that simulated persistence by copying the
serializer/parser into the test. Focused model, migration, recovery, concurrency,
and media-engine suites remain in place.

## Prove the tests reject broken behavior

Run `./tests/backend/verify_mutations.ps1`. It independently disables bookmark
extension preservation, settings extension preservation and cache budget
enforcement in disposable source copies. Each must fail its specific assertion;
compilation errors and unrelated failures do not count. It never edits the real
source tree. This slower check is manual, rather than recompiling three mutated
backends on every routine gate.

## Measure performance

`./tests/backend/run_benchmark.ps1 -OutputDirectory <output>` measures 2,000 actual
cache files and 100,000 fractional numbers. Use `-SourceDirectory <baseline/src>`
to compile the same harness against a saved baseline. Each workload discards one
warmup and reports seven samples, output hashes and C++ heap allocations. Compare
matching hashes before comparing times. Run versions sequentially without other
compilation; repeat in reversed order when timing changes are small or noisy.

`tests/benchmark/run_save_benchmark.ps1` additionally measures library save/load.
Its `retained_cpp_bytes` column counts tracked C++ allocations still alive at the
end of the operation, including the loaded library for the load workload. These
allocation counts are not process working set or GPU memory. The application
startup/resource gate and real playback workflows cover those separate paths.
