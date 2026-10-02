# Evaluation implementation tracking

Scope: implement all actionable recommendations in `EVALUATION_2026-10-02.md`, preserving functionality, visual design, portable data, and the local build/release policy. No release or upload is requested. Speculative migrations require measurement before selection; preserve dormant seekbar functionality and allocate its resources lazily.

Completion requires source evidence, appropriate focused tests, the local regression gate, and a requirement-by-requirement audit. "Implemented" below records source and component validation; native integration checks and the final regression gate remain required before the overall goal is complete.

| Finding | Required change | Status / evidence |
|---|---|---|
| F01 | Error-aware root/entry availability; non-destructive incomplete scans; honor deferred-save intent | Implemented in PathFileHelpers / MediaLibrary reconciliation; offline-root and deferred-save regressions pass |
| F02 | Central playback privacy policy; prevent history/library/session/cache mutations for private opens | Implemented PlaybackPersistencePolicy and App history/progress/close/recent-stream guards; policy regression passes; App boundary check pending |
| F03 | Reacquire target identity after reordering | Implemented in OpenAndPlayMedia; App boundary check pending |
| F04 | Typed, bounded rule validation at edit/load; safe evaluation | Implemented cached numeric parsing, editor validation, disabled invalid loaded rules, corrected operator parsing; numeric regressions pass; JSON structural/cast audit remains |
| F05 | Structural store and backup validation; future-schema write protection | Implemented collection/schema validation and future-version fences; recovery and preservation tests pass; per-store save scoping will be retained in F07 |
| F06 | Normalized volume and persisted roundtrip | Implemented CLI/automation normalization and Settings volume roundtrip; regression passes |
| F07 | Per-store dirty detection before serialization; coalesced serialized background saving with final flush and errors | Pending |
| F08 | Bounded retry of unresolved metadata; shared eligibility predicate | Implemented shared eligibility/backoff predicate and zero-duration retry accounting; regression passes |
| F09 | Route UIA root requests; verify native provider patterns | Implemented WM_GETOBJECT routing; provider component tests pass; native root-request check pending |
| F10 | Distinguish recoverable operation errors from fatal playback failures | Implemented separate warning/fatal mpv callbacks and UI handling; external-subtitle regression passes; native playback check pending |
| F11 | Distinguish HTTP interruption from clean completion | Pending |
| F12 | Async subtitle search/download and native updater fallback; generation-safe results; reuse results | Pending |
| F13 | Failed-load preservation fences and surfaced errors for settings/session | Implemented failed/unavailable/future-store fences and initialization error reporting; reset/load preservation tests pass |
| F14 | Guaranteed recovery copy or explicit failure; checked rotation; aligned read/write limits | Implemented protected newest backups, checked access/rotation/caller status, shared 64 MiB limit; large-backup and write-limit regressions pass |
| F15 | Serialize MF lifecycle transitions; pairing test | Implemented lifecycle locking and counters; 16-worker pairing regression passes |
| F16 | Bounded single-instance readiness/delivery and useful failure result | Implemented ready event, 10 s discovery deadline and bounded delivery; application builds; integration check pending |
| F17 | Explicit placement validity; negative-coordinate restore | Implemented hasWindowPlacement and legacy migration; negative-coordinate roundtrip passes; native restore check pending |
| F18 | Device generation invalidates external preview resources; checked upload | Implemented D2D device identity binding before view-model construction and checked bitmap uploads; actual two-device WARP regression passes |
| F19 | Measure overlay cadence/cost; display-paced/content-aware redraw; retain transparency/hit testing | Pending |
| F20 | Stream/local persistence distinction, opt-in/redaction/private policy | Implemented typed target classification, local session filtering and recent-stream privacy guards; policy/token regression passes; legacy library/history scrub audit remains |
| F21 | Availability-aware missing rule without unnecessary file checks | Implemented conditional availability queries and 2 s cache refresh for missing criteria; direct availability regression passes |
| F22 | Responsive cancellation/close with safe owned worker lifetime and interruption strategy for external APIs | Pending |
| F23 | Separate lexical/index and metadata generations; history membership invalidation | Pending |
| F24 | Effective redirect limit/final URL and explicit destination policy | Pending |
| F25 | Move HTTP body; reusable buffer; streaming/hash staging for large packages | Pending |
| F26 | Lazy seekbar bitmap/save worker; preserve existing hover and dormant extraction API | Implemented lazy preview buffers/bitmaps and first-save worker startup; dormant extraction cache regression passes; native hover check pending |
| F27 | Safe reclaim of retired IOCP watch objects after final completion | Implemented outstanding-read ownership, completion reclamation and shutdown draining; 128-cycle removal/restart regression passes |
| F28 | Successful, visible presentation gates startup milestones | Implemented EndDraw/occlusion gate; application builds; benchmark/native present check pending |
| F29 | Checked mpv options with essential/optional policies | Pending |
| F30 | Shared typed DLL/API ownership and checked option helper; retain separate handles/policies | Pending |

Validation plan: focused component and App-boundary tests; one before/after resource comparison; local application build and regression gate; targeted native UIA, placement, private playback, device recreation, network fault, and worker-cancellation checks. Record exact commands/results as they finish. Keep meaningful changes in descriptive local commits.

## Validation recorded

- `tests/unit/run_tests.ps1 -Suite EvaluationFixes`: 15/15 passed before the additional device/watch tests.
- `tests/unit/run_tests.ps1`: 352/352 passed, including all 17 new EvaluationFixes cases, in 3954.98 ms of test execution.
- `build.ps1 -BuildOutputPath scratch/evaluation-fixes/Aperture.exe`: complete Release application build passed (52 compile/link tasks, binary 2,970,112 bytes). This isolated diagnostic build does not install or publish a package. The required ordinary build/package and final regression gate will use `build_and_release.ps1` once all changes are ready.
- The first deferred-save overload and missing-rule parsing failures were corrected; the complete component run above includes their regression coverage.
