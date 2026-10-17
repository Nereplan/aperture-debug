# Backend balance evidence

See [the report](../../reports/BACKEND_BALANCE_20261017.md) for scope, methods,
source identity and limitations. All measurements were local.

- Capture CSVs: 14 measured samples per workload/version, excluding sample zero.
- Engine JSONs: one warmup and three measured rotated rounds.
- Application JSONs: fresh portable profiles, verified advancing playback.
- Startup JSONs: five measured samples; both budget gates passed.
- Logs: original output with line endings and trailing whitespace normalized.
- The old-reader failure is retained alongside the corrected workflow pass.
- Source identity distinguishes the working-tree baseline from clean Git HEAD.

Large binaries, media, profiles and build trees are not checked into this evidence
directory. Generated cleanup was blocked by automatic approval policy; the
development scratch copies remain, outside the distributed runtime package.
