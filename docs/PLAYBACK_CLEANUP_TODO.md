# Playback, Hub and cleanup checklist

Work order: fix UI and interaction behavior, validate it locally, commit a save
point, then remove unused code and disposable build artifacts. Existing worktree
edits and historical performance evidence must be preserved. Tests stay hidden;
no fresh VLC baseline or visible playback benchmark is needed for this work.

## 1. Playback and Hub behavior

- [x] Restore Space play/pause after opening or closing playback settings;
      preserve normal typing inside the new speed entry.
- [x] Make hover/click bounds agree with drawn rows in Queue, Aspect Ratio,
      Playback Speed, Audio Track and Subtitles. Blank space must stay inert.
- [x] Allow captured, continuous playback-speed slider dragging.
- [x] Add a directly editable playback speed; keep the flyout open after presets.
- [x] Remove the layered flyout shadow and verify readable rendering.
- [x] Make toolbar CC toggle subtitles off/on while retaining track selection.
- [x] Remove the redundant Content Speed Presets context-menu section.
- [x] Make Add Bookmark work entirely during playback.
- [x] Replace the three A/B actions with one Start / End / Clear cycle, show A/B
      timestamps and state colors, and use Shift+A for the complete cycle.
- [x] Extend Hub Ctrl+Z to reversible edits, including rename and reordering.
- [x] Reopen an existing context menu at the new right-click location, including
      when a submenu is open.
- [x] Add focused behavioral checks, build locally and inspect hidden render
      captures of the changed playback menus.
- [x] Commit the validated UI save point before cleanup.

## 2. Workspace and dead-code cleanup

- [x] Measure directory sizes and separate source, dependencies, Git history,
      generated build/cache output, saved user data and historical evidence.
- [x] Audit production entry points, build inputs and references before deleting
      unused functions/types/files; preserve actual feature behavior.
- [x] Remove redundant code and consolidate repeated logic where it improves
      correctness or maintainability; retain meaningful test coverage.
- [x] Remove only verified disposable generated files; keep source, runtime
      dependencies, user profiles, backups and performance evidence.
- [x] Report actual disk and source-line reductions, with before/after counts.

## 3. Further efficiency and final validation

- [x] Investigate remaining avoidable work found during the cleanup and implement
      justified improvements without reducing playback quality.
- [x] Run checks appropriate to the changed code and a local regression gate.
- [x] Append validation/optimization findings to docs/checkpoints.html when ready;
      retain exact measurement scope and do not invent new performance results.
- [x] Audit every request against current code, test and render evidence, then
      commit the remaining changes and report any real verification limitation.

## Progress

- Initial worktree snapshot: external Codex scratch directory
  `aperture-playback-cleanup-20261019`; existing edits are not this task's changes.
- Starting source commit: bd81ac6. Referenced screenshots inspected.

- UI save point: `f68bc4c`; exported source built and passed 22 hidden checks.
- Final validation: 22/22 PR suites, 25/25 hidden App checks, 5/5 compactor
  contracts; one visible-window unit case remains explicitly skipped.
- Workspace: 4.63 GB to 1.31 GB. Cleanup removed 113 source lines; the requested
  UI/undo additions make the complete task net +221 production source lines.
- Raw scope, hashes, before/after counts and remaining external temporary copies
  are recorded in `docs/evidence/workspace-cleanup-20261019/audit.json`.
