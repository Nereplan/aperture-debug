# Evaluation implementation tracking

Scope: implement all actionable recommendations in `EVALUATION_2026-10-02.md`, preserving functionality, visual design, portable data, and the local build/release policy. No release or upload is requested. Speculative migrations require measurement before selection; preserve dormant seekbar functionality and allocate its resources lazily.

All items start pending. Completion requires source evidence, appropriate focused tests, the local regression gate, and a requirement-by-requirement audit.

| Finding | Required change | Status / evidence |
|---|---|---|
| F01 | Error-aware root/entry availability; non-destructive incomplete scans; honor deferred-save intent | Pending |
| F02 | Central playback privacy policy; prevent history/library/session/cache mutations for private opens | Pending |
| F03 | Reacquire target identity after reordering | Pending |
| F04 | Typed, bounded rule validation at edit/load; safe evaluation | Pending |
| F05 | Structural store and backup validation; future-schema write protection | Pending |
| F06 | Normalized volume and persisted roundtrip | Pending |
| F07 | Per-store dirty detection before serialization; coalesced serialized background saving with final flush and errors | Pending |
| F08 | Bounded retry of unresolved metadata; shared eligibility predicate | Pending |
| F09 | Route UIA root requests; verify native provider patterns | Pending |
| F10 | Distinguish recoverable operation errors from fatal playback failures | Pending |
| F11 | Distinguish HTTP interruption from clean completion | Pending |
| F12 | Async subtitle search/download and native updater fallback; generation-safe results; reuse results | Pending |
| F13 | Failed-load preservation fences and surfaced errors for settings/session | Pending |
| F14 | Guaranteed recovery copy or explicit failure; checked rotation; aligned read/write limits | Pending |
| F15 | Serialize MF lifecycle transitions; pairing test | Pending |
| F16 | Bounded single-instance readiness/delivery and useful failure result | Pending |
| F17 | Explicit placement validity; negative-coordinate restore | Pending |
| F18 | Device generation invalidates external preview resources; checked upload | Pending |
| F19 | Measure overlay cadence/cost; display-paced/content-aware redraw; retain transparency/hit testing | Pending |
| F20 | Stream/local persistence distinction, opt-in/redaction/private policy | Pending |
| F21 | Availability-aware missing rule without unnecessary file checks | Pending |
| F22 | Responsive cancellation/close with safe owned worker lifetime and interruption strategy for external APIs | Pending |
| F23 | Separate lexical/index and metadata generations; history membership invalidation | Pending |
| F24 | Effective redirect limit/final URL and explicit destination policy | Pending |
| F25 | Move HTTP body; reusable buffer; streaming/hash staging for large packages | Pending |
| F26 | Lazy seekbar bitmap/save worker; preserve existing hover and dormant extraction API | Pending |
| F27 | Safe reclaim of retired IOCP watch objects after final completion | Pending |
| F28 | Successful, visible presentation gates startup milestones | Pending |
| F29 | Checked mpv options with essential/optional policies | Pending |
| F30 | Shared typed DLL/API ownership and checked option helper; retain separate handles/policies | Pending |

Validation plan: focused component and App-boundary tests; one before/after resource comparison; local application build and regression gate; targeted native UIA, placement, private playback, device recreation, network fault, and worker-cancellation checks. Record exact commands/results as they finish. Keep meaningful changes in descriptive local commits.
