# Overlay cadence and content invalidation

The F19 implementation preserves the BGRA premultiplied-alpha DIB, layered window, drawing layout, and native hit testing. It adds a coalescing frame deadline at `min(display refresh rate, 60 Hz)`, an owned high-resolution waitable timer with a normal-timer fallback, and exact subtitle-text invalidation for playback with hidden controls. Pending work survives input bursts, the final animation tick, font/DPI changes, and failed presentation. mpv video presentation remains independent. The extra transparent clear in `UIComponents::DrawPlayerOverlay` is removed because `OverlayRenderer::BeginDraw` already clears the surface.

`OverlayFramePacer` provides deterministic coverage for request coalescing/final frames, delayed frames without catch-up bursts, refresh-rate changes, invalid rate fallback, stable subtitle cues, cue replacement, and cue disappearance. All three focused tests passed. They are also registered in the full unit gate.

The native validation uses the actual `OverlayRenderer` and checks initialization, a signaled frame deadline, immediate input coalescing, retry state after `UpdateLayeredWindow` fails, and monitor-mode discovery. Eight initialize/shutdown cycles kept process handles at 353 and GDI objects at 6. One monitor was available: a real transition between two monitors remains untested; the deterministic 120 Hz/30 Hz deadline changes cover the scheduler's rate-change behavior.

Run the native probe locally with:

```powershell
& .\tests\benchmark\run_overlay_cadence_probe.ps1
```

The measured desktop was 1920×1080 at 165 Hz. The tested surface was 3840×2160 (33,177,600 bytes), extending beyond that desktop. The probe uses the production DC/DIB/layered renderer with representative controls or a stable subtitle cue; it excludes mpv decoding, application startup, and library work. Both scheduling modes use the current renderer, so the comparison isolates the old 8 ms wait schedule versus the new timer schedule. It is not a comparison between complete old and new application binaries. Three two-second samples per mode were collected with compiler work paused.

| Mode | Median presented frames/s | Median process CPU ms | Median draw + present ms |
| --- | ---: | ---: | ---: |
| Controls, old 8 ms wait schedule | 32.137 | 921.875 | 1,223.843 |
| Controls, timer deadline | 55.685 | 1,296.875 | 1,985.581 |
| Stable subtitle, old continuous redraw | 32.144 | 906.250 | 1,189.464 |
| Stable subtitle, content invalidation | 0 | 15.625 | 0 |
| Hidden controls, no subtitle | 0 | 0 | 0 |

All 15 samples had zero presentation failures. The live-controls case rendered more frames and used more CPU: these results establish smoother scheduling and a bounded target rate, **not a CPU reduction for continuously changing controls**. The old fixed wait adds latency after an already expensive full-surface draw. Stable subtitle cues no longer redraw or upload that surface until the cue changes. Hidden controls without subtitles already avoided uploads and retain that behavior. Process CPU values vary substantially and have 15.625 ms quantization; they support the large unchanged-cue reduction, not a precise power estimate.

The component probe polls unchanged subtitles every 64 ms. The integrated application retains a 16 ms tick while playing an app-rendered subtitle track so content invalidation does not increase cue timing latency. The probe's subtitle CPU figures therefore describe isolated renderer work rather than complete application CPU consumption.

The 4K layered surface still has significant per-frame cost and keeps its full allocation. These measurements do not justify claiming reduced GPU bandwidth, power use, or DIB memory, nor do they supply the visual/performance comparison required for a DirectComposition migration. That architectural migration remains an opportunity rather than an implemented change.

Raw data: [timing CSV](overlay-results.csv), [median summary](overlay-summary.json), [native validation](overlay-native-validation.json).

The platform behavior follows Microsoft's documentation for [high-resolution waitable timers](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw) and [whole-window layered updates with an optional destination DC](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-updatelayeredwindow).
