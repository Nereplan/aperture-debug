# Custom libmpv builds

Normal Aperture builds use the bundled `libmpv-2.dll`. Its version, SHA-256, size, license identifiers, and upstream URL are recorded in `scripts/dependencies.json`. `scripts/check_dependencies.ps1 -Validate` verifies the bundled file and public headers before each application build.

The bundled engine is **mpv v0.41.0, client API 2.5, 25,240,576 bytes**. The source archives for mpv, FFmpeg n8.0, libplacebo v7.360.1, shaderc, glslang and the SPIR-V dependencies are SHA-256 pinned in `scripts/libmpv/sources.lock.json`. Public headers come from that mpv tag.

`scripts/build_lean_libmpv.ps1` rebuilds the complete engine in MSYS2 **MINGW64/MSVCRT**. Prepare GCC/G++, CMake, Meson, Ninja, NASM, pkgconf, Make, curl, Python 3.12+, and the static dav1d, libass, FreeType, HarfBuzz, FriBidi, Fontconfig, uchardet, lcms2, iconv, xxhash, zlib, bzip2 and xz development packages. The script uses existing packages and records their versions; it does not update MSYS2.

```powershell
./scripts/build_lean_libmpv.ps1 -Jobs 12
# Rebuild dependencies after changing the recipe/toolchain; keep source downloads:
./scripts/build_lean_libmpv.ps1 -Jobs 12 -Clean
```

The default output is `scratch/libmpv-build/output/libmpv-2.dll`, accompanied by `build-provenance.json`, the package inventory and PE inspection. `-OutputDir` is honored and must stay below `-BuildRoot`; the build root must stay below this repository's `scratch/`. A lock prevents concurrent builds, and a stable temporary drive alias handles spaces in paths. This alias points only to the build directory and is removed in `finally` on normal completion or failure; it does not copy or modify another disk. Input changes invalidate cached dependency builds. The recipe stages its output for review; replacing the bundled DLL is a separate step.

Size settings are `-Os`, FFmpeg `--enable-small`, and LTO for FFmpeg, libplacebo and mpv, including explicit `-Os` at the final link. The shader chain uses `-Os` without LTO because GCC 16.2 encounters an internal compiler error there. Assembly, CPU dispatch, D3D11 hardware decoding, WASAPI, libass, colour management and the existing codec/container/protocol allowlists remain enabled. `dynaudnorm` and `pan` restore the missing night-mode and channel-map filters. Avoid `-fdata-sections`: this MinGW toolchain emits FFmpeg's large zero-initialized tables as file data with that flag.

The optional libdovi dependency is disabled; FFmpeg Dolby Vision metadata parsing and libplacebo reshaping remain compiled in. HDR10 decoding has been exercised, but Dolby Vision presentation and HDR display quality require appropriate media and hardware. The old DLL's exact source revision cannot be proven from its version string and imports; the new build's source provenance is explicit.

Run the manual playback suite after an engine change:

```powershell
./tests/libmpv/run_tests.ps1
# Optional loopback HTTPS/RTSP/RTMP fixture (provide MediaMTX, FFmpeg, OpenSSL, Python):
./tests/libmpv/run_network_tests.ps1 -MediaMtxPath C:/tools/mediamtx.exe
```

Playback tests compile an isolated harness, copy the selected DLL, verify its loaded absolute path and file hash, and fail on any failed scenario or timeout. Generated media and logs live in `scratch/libmpv-tests/`. Use `-DllPath`, `-MediaDir`, and `-Scenario` for comparisons; `bench` and `startup` are separate performance scenarios. The legacy `balance` property is recorded as a diagnostic because both original and new v0.41.0 engines report it missing. The self-signed loopback HTTPS fixture bypasses certificate verification only in its test client; `-TrustedHttpsUrl` separately tests a normally trusted endpoint with verification enabled.

The integration results and limitations are in [LIBMPV_INTEGRATION_REPORT.md](LIBMPV_INTEGRATION_REPORT.md). Source/flags and the captured toolchain support rebuilding the same configuration; timestamps and toolchain differences can change the binary hash.

When updating libmpv, update the matching headers in `include/mpv/` and the dependency inventory, run the loader and playback tests, and create a new complete release package. The portable updater distributes the DLL, fonts, and locale catalogs together with the application executable.
