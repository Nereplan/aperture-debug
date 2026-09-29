# Custom libmpv builds

Normal Aperture builds use the bundled `libmpv-2.dll`. Its version, SHA-256, size, license identifiers, and upstream URL are recorded in `scripts/dependencies.json`. `scripts/check_dependencies.ps1 -Validate` verifies the bundled file and public headers before each application build.

`scripts/build_lean_libmpv.ps1` is an optional MSYS2 recipe for maintaining a custom playback DLL. It is not part of the normal build or release workflow. Read its configuration and prerequisites before using it; replacing the playback engine requires independent playback and dependency validation.

When updating libmpv, update the matching headers in `include/mpv/` and the dependency inventory, run the loader and playback tests, and create a new complete release package. The portable updater distributes the DLL, fonts, and locale catalogs together with the application executable.
