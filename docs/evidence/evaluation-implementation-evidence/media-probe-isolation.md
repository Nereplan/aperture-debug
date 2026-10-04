# F22: external media probe isolation and cancellation

The App import/backfill workers install `ScopedMediaProbeIsolation`; `MediaLibrary::ProbeMediaMetadata` routes their calls to a reusable child process. Renderer thumbnail workers use the same bounded protocol for Shell cache lookup, `GetImage` extraction and WIC cubic downscaling. Existing local disk caching, private-mode write suppression, metadata merging, thumbnail result generations and mpv thumbnail fallback remain in the parent.

The helper uses the current Aperture executable, started before UI/single-instance initialization through an internal entry point. Only its unnamed mapping/request/response handles are inherited through `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`. It is assigned to a non-inherited `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` job while suspended and then resumed. Request/response sizes, paths, dimensions and sequence/version fields are checked. The child contains no App/Renderer pointers. Cancellation polls at 25 ms and terminates only that process; synchronous MF/Shell calls do not need an unsupported in-process cancellation or unsafe thread termination. Normal teardown gives MF/COM a bounded graceful shutdown opportunity.

Normal UI cancellation signals its owner and reaps it after completion, rather than joining immediately. Root App close integration cancels all owners and keeps the message loop alive until their completion flags permit final teardown. Completion flags and renderer worker counts have RAII publication after helper destruction, including constructor/allocation failures. `CancelSynchronousIo` also requests interruption of parent-side filesystem operations. D3D creation is monitored by a completion flag; there is no claim that a blocked graphics driver or arbitrary kernel I/O has an unconditional termination contract.

The native suite (`tests/native/run_media_probe_isolation.ps1`, registered in the local `pr` gate) verifies actual MF PCM WAV metadata, actual Shell BMP thumbnail/downscale, process reuse, confirmed entered blocked-operation cancellation/deadline, process exit and recovery, input limits, failed-launch handle cleanup, scoped routing and actual idle helper exit/restart. Its deterministic frozen operation exists only under `APERTURE_TEST_PROBE_ISOLATION`; a deliberately hung third-party codec/extension is not installed. The blocked operation uses the same helper ownership, wait and cancellation path. A one-time failed launch warms Windows' observed lazy failed-CreateProcess state before the test verifies that 20 subsequent failures add no handles; no leak threshold is permitted.

Measured on the local machine against `scratch/evaluation-fixes/Aperture.exe` (Release, 3,104,768 bytes), with generated local WAV/BMP fixtures and no competing compiler. These are individual diagnostics, not cross-machine budgets:

| Measurement | Result |
| --- | ---: |
| Confirmed blocked helper deadline | 516 ms for a 500 ms configured deadline |
| Cancellation after confirmed helper entry | 78 ms |
| First metadata probe including helper launch | 120.176 ms |
| Reused helper metadata probe | 1.357 ms |
| One MF helper private bytes | 3,969,024 |
| One MF helper working set | 19,824,640 |
| Four simultaneously resident Shell-only helpers, aggregate private bytes | 14,700,544 |
| Observed idle process exit | 5,017.050 ms |

Isolation trades transient helper startup/private memory for bounded cancellation of extension/codec calls. Renderer creates at most four helpers on actual cache misses; import/backfill can each add one. Helpers exit after five seconds without a request and restart on later demand. Working sets include shared DLL pages, so summing working sets would overstate independent physical memory. Protocol mappings are demand paged with a fixed four MiB pixel ceiling, and normal canonical thumbnails remain 320x180/640x360.

Raw measurements are in `media-probe-isolation-results.csv`. Microsoft documents that [Shell extraction belongs off the UI thread](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishellitemimagefactory-getimage), and that [closing the final kill-on-close job handle terminates associated processes](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects).
