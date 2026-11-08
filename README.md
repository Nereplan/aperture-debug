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

## Local releases and change history

Builds, tests, and packaging run locally. This repository has no GitHub Actions workflows; pushing commits or tags does not start runners. GitHub stores the source history and downloadable releases. Build outputs and personal data are excluded by `.gitignore`.

For a future release, run `./scripts/set_version.ps1 -Version 2.0.1.0` with your next version, make descriptive commits, and push the source. Then run `./scripts/publish_release.ps1` on your Windows machine (GitHub CLI and `gh auth login` required). It builds, runs regression tests, packages locally, and uploads the ZIP and checksum to GitHub Releases. Each release points to the exact source commit and lists commits since the previous published release. Existing versions are never overwritten.

Use `./scripts/publish_release.ps1 -PrepareOnly` to build and write release notes in `dist/` without contacting GitHub. Publishing requires a clean checkout and a source commit already pushed to the repository. No release is needed for the current commit.

For an existing source checkout, **Update Source.cmd** fast-forwards from its configured upstream, builds, tests, and updates your local apps. It stops if source changes are uncommitted or the branches have diverged.

Open the [performance checkpoint dashboard](docs/checkpoints.html) to compare Aperture revisions with measured VLC baselines, inspect benchmark graphs and validation, or read historical reports. The single HTML works offline; [checkpoint instructions](docs/CHECKPOINTS.md) explain how agents add new measurements.

See the [documentation index](docs/README.md) for build, testing, portable-data, engine-build, and measured optimization references.

## Linux Port (CachyOS / Arch Linux)

Aperture also provides an independent native Linux x86_64 port (`src/linux/`) sharing the core C++17 UI and playback codebase so features and updates apply to both Windows and Linux simultaneously.

### Runtime Dependencies (CachyOS / Arch Linux)

```bash
sudo pacman -S mpv sdl2 cairo pango glib2 gdk-pixbuf2 curl dbus openssl
# Optional (recommended for network streams, thumbnails, and native file dialogs):
sudo pacman -S yt-dlp ffmpeg kdialog zenity
```

### Option 1: Portable `.tar.gz` Release

Extract `dist/Aperture-<version>-linux-x86_64.tar.gz` to any writable folder and run `./aperture`.

- **Hybrid Data Storage & Windows-to-CachyOS Migration**: If a writable `data/` folder exists beside the `aperture` binary, Aperture runs in portable mode and stores your library, playlists, settings, and watch history in `./data/` (you can copy your Windows `data/` folder directly into the portable Linux folder when migrating to CachyOS). When installed system-wide to `/usr/bin/aperture`, it automatically stores user data in `$XDG_DATA_HOME/aperture` (`~/.local/share/aperture`).

### Option 2: Native CachyOS / Arch Package (`PKGBUILD`)

From a CachyOS or Arch Linux terminal:

```bash
cd packaging/arch
makepkg -si
```

This builds and installs `/usr/bin/aperture`, `/usr/share/applications/aperture.desktop`, `/usr/share/aperture/fonts/`, and `/usr/share/aperture/locales/`.

### Building the Linux Port

- **On CachyOS / Linux (Native CMake + Ninja)**:
  ```bash
  ./build_linux.sh              # Builds dist/linux-x86_64/aperture
  ./build_and_release_linux.sh  # Builds and packages dist/Aperture-<version>-linux-x86_64.tar.gz
  ```
- **On Windows (Cross-compile to Linux x86_64 ELF + `.tar.gz`)**:
  ```powershell
  ./build_linux.ps1
  ```
