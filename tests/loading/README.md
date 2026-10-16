# Portable library loading regression suite

Run `./tests/run_tests.ps1 -Id loading` or `./tests/loading/run_tests.ps1`.
The suite is in the local PR gate and runs eight individually selectable cases
against production code. `-Test <name>` selects a case; unknown selections fail.

- Streaming JSON matches the DOM parser, rejects truncated/trailing data, and
  preserves nesting, decoded-string and element-count limits.
- Actual array and versioned stores produce identical portable serializations,
  including Unicode, bookmarks, preferences and unknown extensions. Reload and
  directory relocation retain their data and repair lookups.
- A malformed suffix discards all partially decoded records. A real backup can
  recover the library; unrecoverable and future-version stores remain write-protected.
- Incremental saves leave the checkpoint unchanged, replay after restart, reject
  corrupt journals, and survive explicit checkpoint publication.
- Heap probes compare streaming and compatibility loads of 6,000 records. Sparse
  records require at least 5% less peak tracked C++ heap; metadata-heavy records
  may not regress more than 5%. Retained allocation slack is checked separately.
  These are resource contracts, not wall-clock timing or whole-process RAM tests.

The runner creates an isolated executable directory and all tested writes stay
inside it, including stores and backups. Cleanup removes that installation.
`-KeepArtifacts` retains it for investigation. No installed user data is used.

Run `./tests/loading/verify_mutations.ps1` to introduce two defects in disposable
source copies: accepting trailing JSON and disabling streaming. Each must fail
its specific behavioral/resource assertion. Compilation failures do not count.

Use `tests/benchmark/run_save_benchmark.ps1` for before/after performance samples.
Its source-directory option allows the same harness to exercise a saved baseline;
compare output hashes and repeat in reversed order before interpreting timings.
