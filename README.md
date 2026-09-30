# Aperture

A portable media player for Windows 10/11 x64, built with C++17, Win32, Direct2D, and libmpv.

## Run

Download the ZIP from this repository's Releases page, extract it to a writable folder, and open `Aperture.exe`. No installation is required.

Your library, playlists, settings, watch history, and playback positions are stored in `data/` beside the app. Keep this folder when moving Aperture to another location.

## Update

Choose **Settings > System & Storage > Update Aperture**, or double-click `Update.cmd` beside the executable. The updater downloads the latest stable GitHub release, verifies its checksum and file manifest, closes Aperture normally, installs the runtime files, and reopens it. Saved data and custom `mpv.conf` are retained. The last three successful pre-update snapshots are kept in `update-backups/`.

GitHub updates become available after the first release is published from your repository. Local builds without a repository show an explanatory message.

## Build

Install Visual Studio 2022 with Desktop development with C++, the Windows SDK, CMake, and Python 3. The playback DLL, headers, and fonts are included; `scripts/dependencies.json` records their checksums and origins.

Double-click **Build Release.cmd** to build, run the regression checks, create a distributable ZIP in `dist/`, and update the local `release/` and `debug/` apps. Each app keeps its own saved data. The default working app is optimized; use the following for a debug build with symbols:

```powershell
./build_and_release.ps1 -IncludeDebug
```

To compile only, use `./build.ps1` or `./build.ps1 -Debug`. Local builds reuse a configuration-specific cache in `scratch/build/`; `./build.ps1 -Clean` performs an isolated full rebuild. Analysis and sanitizer builds remain isolated. To run regression tests, use `./tests/run_tests.ps1 -Tier pr`.

## GitHub workflow

Create a repository and push this source tree to its `main` branch. Build outputs and personal data are excluded by `.gitignore`. Commits and pull requests run Windows CI.

To publish, run `./scripts/set_version.ps1 -Version 2.0.1.0` with your next version, commit and push, then choose **Actions > Release > Run workflow**. Alternatively, push a tag matching the four-part version, such as `v2.0.1.0`. The workflow tests and builds the app, packages it with the correct repository address, and publishes the completed release.

For an existing source checkout, **Update Source.cmd** fast-forwards from its configured upstream, builds, tests, and updates your local apps. It stops if source changes are uncommitted or the branches have diverged.

See [Build and release](docs/BUILD_AND_RELEASE.md), [portable data](docs/PORTABLE_DATA_MIGRATIONS.md), and [custom libmpv builds](docs/CUSTOM_MPV_BUILD.md).
