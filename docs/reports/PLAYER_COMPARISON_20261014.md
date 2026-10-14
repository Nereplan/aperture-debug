# Player comparison and preview work reduction — 14 October 2026

Aperture has a small runtime and shows its window quickly in this local comparison.
VLC and MPC-HC use less memory during the tested playback workload. Reviewing their
preview pipelines identified a useful change: keep seekbar extraction paused and
stop processing once the requested thumbnail has been captured.

## Matched playback measurements

These are measurements on this machine, not published vendor benchmarks. Aperture
is the clean `cee51e3` runtime, version 2.0.0.0, **before** the preview change below.
Its existing uncommitted frontend changes were present in that build and preserved
byte for byte throughout this work. VLC is the installed 3.0.23 distribution copied
into an isolated portable runtime; MPC-HC is the official 2.8.3 x64 ZIP.

| Metric, median of three measured runs | Aperture | VLC 3.0.23 | MPC-HC 2.8.3 |
|---|---:|---:|---:|
| Process launch to visible window | 111.8 ms | 1,679.5 ms | 726.1 ms |
| Playback working set | 163.9 MiB | 143.0 MiB | 114.4 MiB |
| Playback private bytes | 255.8 MiB | 145.0 MiB | 142.4 MiB |
| Process dedicated GPU allocation | 173.3 MiB | 117.9 MiB | 101.9 MiB |
| Process shared GPU allocation | 15.6 MiB | 15.2 MiB | 17.4 MiB |
| Complete supplied runtime, logical bytes | 28.4 MiB | 183.0 MiB | 72.0 MiB |
| CPU use, percent of one logical core | 27.6% | 3.7% | 2.1% |

The startup endpoint is **a visible window, not the first video frame**. Working
set is resident process memory; private bytes represent private committed memory.
GPU counters report the driver's per-process allocations. These categories overlap
and must not be added into a single RAM/VRAM total. The preview optimization does
not establish a reduction in these normal-playback memory figures.

The full VLC directory includes its languages, plugins and installer-related
payload. MPC-HC includes its shipped filters, renderers and languages. Aperture
includes its manifest, fonts, locales and update tools. These are complete supplied
distributions, not feature-matched minimal builds or compressed download sizes.
Median app-directory growth during a run was 26,242 / 419 / 21,160 bytes respectively;
redirected profile directories had no file growth. This is a short observation of
the owned directories, not an OS-wide filesystem trace.

### Workload and controls

- Ryzen 9 3900X, 24 logical processors, approximately 32 GiB RAM, RTX 4070 SUPER,
  driver 32.0.16.1088, Windows 11 Pro 10.0.26200.
- One generated 60-second H.264/yuv420p 1920×1080, 30 fps clip, without audio or
  subtitles, 44,681,659 bytes. SHA-256:
  `7e171361fa8611a504a439d4452d63fcd2c5da6dc4307853f9c114ffa1e37dc3`.
- One discarded warmup plus three measured rounds per player; order rotated.
  Each run used a fresh runtime/configuration copy. OS file caches were warm.
- All final outer window rectangles were 1280×800. The harness reapplies the size
  after loading because MPC-HC initially resizes to the video. Earlier exploratory
  runs with mismatched dimensions are excluded from the table.
- Sampling began eight seconds after window creation plus two seconds after the
  post-load resize. Three samples were separated by at least two seconds; GPU and
  status collection added overhead. Memory figures are the median sample per run,
  followed by the median across runs. CPU is the sampled CPU-time/wall-time slope.
- All twelve runs verified advancing playback: authenticated Aperture IPC,
  VLC's owned localhost RC socket, and MPC-HC's owned `/slave` message window.
  Root-process counters exclude the harness. No heavy build ran during measurement.
- MPC-HC seek preview, history, update checks and enhanced taskbar features were
  disabled. VLC history/update prompts were disabled. No preview was requested in
  any player. Renderer/decoder defaults and actual video viewport dimensions can
  differ; position advancement does not prove pixel quality or identical hardware
  decoding. This test does not cover cold boot, 4K/HDR, audio, subtitles, network
  playback, seeking latency, long sessions or every supported format.

## What the other implementations suggest

| Source reviewed | Structure | Decision for Aperture |
|---|---|---|
| MPC-HC 2.8.3 | A second graph renders seek previews while paused; hover seeks that graph. Repeated positions are suppressed. Preview filters limit queues and disable unused work. | Apply paused one-shot extraction and coalesce repeated requests in the existing libmpv backend. Retain Aperture's exact target checking and bounded thumbnail caches. |
| MPC-BE | Its preview graph rejects audio and subtitle paths. | Aperture already disables audio/subtitles and limits software-preview decoding to two threads; keep those limits. |
| VLC 3.0.x | Plugin metadata caching defers mapping modules until needed. Its executable-adjacent `portable` directory also isolates Qt settings. | Aperture already defers player/preview initialization and ships a much smaller runtime. Adding another plugin framework would not establish a benefit. Use VLC's real portable mode in the comparison. |
| mpv | Configurable forward/backward packet caches and portable configuration support different playback workloads. | Aperture already uses 8 MiB forward/zero backward limits for playback and seek previews. Cache ceilings are not measured allocations; lowering them further does not explain the observed memory gap. |

Primary sources: [MPC-HC preview graph](https://github.com/clsid2/mpc-hc/blob/2.8.3/src/mpc-hc/MainFrm.cpp#L14475),
[MPC-HC hover request suppression](https://github.com/clsid2/mpc-hc/blob/2.8.3/src/mpc-hc/PlayerSeekBar.cpp#L1174),
[MPC-HC preview filter setup](https://github.com/clsid2/mpc-hc/blob/2.8.3/src/mpc-hc/FGFilterLAV.cpp),
[MPC-BE graph filtering](https://github.com/Aleksoid1978/MPC-BE/blob/master/src/apps/mplayerc/FGManager.cpp#L883),
[VLC module loading](https://github.com/videolan/vlc/blob/3.0.23/src/modules/bank.c),
[VLC portable paths](https://github.com/videolan/vlc/blob/3.0.23/src/win32/dirs.c),
[mpv demuxer implementation](https://github.com/mpv-player/mpv/blob/master/demux/demux.c).
MPC-BE and standalone mpv were source-review comparisons only; no execution results
are claimed for them. MPC-HC renders its preview graph directly; its separate
screenshot API is not the seekbar-preview implementation.

## Implemented backend change

Previously a cache miss resumed the preview decoder. Each subsequent rendered
frame could be copied, cached, queued for disk persistence and uploaded under the
same thumbnail timestamp while the pointer stayed still. Later frames could also
replace the requested thumbnail's contents.

`MpvPreviewPlayer` now keeps extraction paused, drains old client events, and waits
for seek restart completion, a non-seeking state and a matching target position.
It performs one capture for the current request. Repeated requests for the same
time bucket reuse the pending or completed request. A cache hit cancels obsolete
pending extraction. Reset, stop and shutdown clear the capture state. An unresolved
request expires after five seconds instead of rendering indefinitely; moving to a
new request or resetting allows another attempt. Animated library previews still
resume and decode continuously.

The implementation adds no dependency, on-disk format or persistent index. Existing
cache ceilings and executable-local data/cache paths remain in effect. Stopping
duplicate work reduces repeated frame copying, uploads and disk-save queueing;
it is not a measured reduction in final cache size or normal-playback VRAM.

### Direct before/after check

The same local 45-second H.264/10 fps fixture and D3D11 WARP/D2D setup produced:

| One-second stationary thumbnail hold | Before (`cee51e3`) | After |
|---|---:|---:|
| Successful bitmap updates after initial capture | 10 | 0 |
| Renderer calls after initial capture | Not instrumented | 0 |

The updated real-libmpv probe also verifies forward and backward target positions,
unchanged cached pixels while held, 100 repeated requests without added seeks,
cached-request cancellation, upload onto a replacement D2D device, animated hover
(five render calls in 500 ms) and hub extraction. A synthetic event sequence rejects
old redraws/incomplete seeks and verifies bounded failed-request behavior. Disk
write counts, power savings and peak-memory deltas were not measured by this probe.

### Final validation

`build_and_release.ps1` built the optimized executable, passed **20/20** regression
suites in **408.78 seconds**, verified the package and updated the workspace
`release/` and `debug/` copies with their saved data retained. This includes the
static-analysis, portable-containment, shutdown-stress and thumbnail-cache gates.
The additional native preview probe passed separately. `git diff --check` passed;
all ten pre-existing modified frontend files remained byte-identical to the initial
snapshot. No frontend changes or GitHub release were made by this work.
The final executable is 3,280,896 bytes (1,024 bytes larger than the measured
baseline); the runtime payload is 29,766,501 bytes and the ZIP is 13,358,672 bytes.

## Reproduction and evidence

See [comparison runner instructions](../../tests/benchmark/README.md) and
[raw evidence](../evidence/player-comparison-20261014/). The evidence includes all
final playback samples, executable hashes, hardware, summaries and preview output.
The baseline preview probe is retained there; compile it with `MpvPlayer.cpp/.h`
from `cee51e3` and the same dependencies as `tests/native/run_preview_pipeline.ps1`.
The final preview check uses that runner directly with `-VideoPath` and optional
`-OutputDirectory`.

The playback clip was generated with:

```text
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30:duration=60 -c:v libx264 -preset veryfast -threads 2 -crf 22 -pix_fmt yuv420p -an -movflags +faststart h264-1080p30.mp4
```

The MPC-HC ZIP came from its [official 2.8.3 release](https://github.com/clsid2/mpc-hc/releases/tag/2.8.3);
its 30,092,251-byte asset matched the published SHA-256
`b7f63edfa2bc5a68936ae126a960fca2630abeb8797d0b95a12227aafe1470c2`.
Competitor binaries/media stay in external development scratch, outside the source
tree. Generated per-run runtime/profile copies were removed after measurement.
An optional final cleanup of the downloaded inputs and baseline runtime copy was
blocked by automatic approval review (reason: “blocked by policy”), so those inputs
remain in the external scratch directory. This does not change Aperture's runtime
storage paths.

The main remaining measured issue is normal-playback private memory, GPU allocation
and CPU use. Decoder surfaces and presentation work need separate attribution before
changing queue sizes or rendering modes; the present measurements do not identify
which component causes that gap.
