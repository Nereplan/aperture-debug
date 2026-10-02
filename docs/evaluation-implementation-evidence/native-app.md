# Native application and preview boundaries

The actual Release application runs in a new, owned temporary portable profile, with generated Unicode PCM WAV fixtures and the bundled `libmpv-2.dll`. The workflow refuses to run against an existing Aperture session. It uses public authenticated IPC and an external Windows UI Automation client; it does not call App internals.

The completed workflow verifies real decoding and advancing/paused clocks, seek accuracy, queue next/previous, stop, fullscreen/Escape/back, three restart/resume cycles, the promoted target's saved playback speed, CLI/automation volume and its persisted restart, a recoverable missing external subtitle, the custom UIA root, negative-X window placement, and private playback of both an existing and a new target. The private tests compare persisted progress, watched state, preferences, history and session after close/shutdown. The final workflow also verifies editable Value, RangeValue with actual player-volume changes, Selection/SelectionItem navigation, and a real Scroll invocation and restoration, plus a hidden second instance forwarding 35% volume with exit code zero. All native assertions pass (225 public IPC commands, p95 latency 65.4934 ms); [workflow result](app-native.json) and [assertion log](app-native.log).

The workflow exposed a Release failure in the shared mpv export-binding path. Production now calls the ordinary imported `GetProcAddress` directly; optional resolver injection remains available for deterministic missing-export tests. The workflow passes after this change. The precise interaction with the local instrumentation/CFG environment was not established, so no specific driver or third-party module is blamed.

Run the application boundary suite with:

```powershell
& .\tests\workflows\run_tests.ps1 -AppOnly -Executable .\release\Aperture.exe -KeepArtifacts
```

The separate native preview probe uses the real bundled decoder and two actual WARP-backed D2D devices. It decodes a 640x360 hover bitmap, exercises the preserved dormant seekbar extraction API, invalidates its old device bitmap, uploads the cached frame on the second device, and decodes a nonempty 320x180 hub thumbnail. All four checks pass; [raw result](preview-native.txt).

```powershell
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=320x180:rate=10 -t 45 -c:v libx264 -preset ultrafast -pix_fmt yuv420p -threads 1 -an -movflags +faststart scratch/evaluation-fixes/native-video.mp4
& .\tests\native\run_preview_pipeline.ps1 -VideoPath scratch/evaluation-fixes/native-video.mp4
```

The startup benchmark passed its checked-in budgets with one measured sample after one warmup: successful visible GPU shell 44.08 ms, window shown 27.05 ms, populated library frame 284.99 ms, working set 55.08 MiB and private bytes 58.93 MiB. [Actual sample](startup-native.json). This proves startup milestone integration on this machine; one sample does not establish a timing distribution or an improvement over the old application.

The mouse-driven UI smoke test successfully launched/opened the generated H.264 video, then could not focus the owned window before its close-video input. The process remained alive and no matching WER events were recorded. Its later mouse/picker/menu actions are therefore unverified. Only one monitor was available: negative-Y placement above a primary monitor and physical monitor-transition behavior remain native-environment limitations; signed-coordinate persistence and rate changes have component coverage. Blocking graphics drivers or arbitrary kernel I/O are not forcibly terminated by the close pump.

The native UIA SelectionItem call initially returned HRESULT 0x8007023e. The provider held its detachment mutex while publishing UIA events, which may synchronously read that provider. Its lock now permits same-thread reentry while still excluding concurrent detachment. The complete actual-client pattern workflow passes with this correction. The final local regression/package gate remains pending.
