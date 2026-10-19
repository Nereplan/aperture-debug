# Hidden playback interaction checks

Run `./tests/playback_ui/run_tests.ps1` locally on Windows after installing the
normal build prerequisites. It builds the current application and links its
production objects into a console harness with a muted native player. Every
owned window stays hidden; the harness fails if any becomes visible. It uses a
fresh portable profile outside the repository and never launches VLC.

The default media is the generated `scratch/libmpv-tests/media/h264_1080p_60s.mp4`;
pass `-MediaPath` for another local, seekable video of at least 60 seconds.
`-OutputDir` must be a new directory below `%LOCALAPPDATA%/Codex/tools/scratch`.
Build logs, source/runtime hashes, check results and PNG captures stay there.

Checks exercise actual App event handlers, native speed/subtitle/A-B controls,
playback bookmark confirmation, menu relocation and Hub undo. Drag checks seed
valid drag state and deliver the normal release event. Keyboard modifiers are
thread-local; no input is injected into the user's desktop. Captures use the
actual overlay bitmap, with menu animation settled for inspection.

This is interaction and rendering validation. Hidden playback does not measure
visible-window CPU/GPU performance, presentation timing or picture quality.
