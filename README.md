# Aperture

Portable media player for Windows (x64) and Linux (x86_64), built with C++17 and `libmpv`.

---

## Windows (x64)

### Run

Extract the release ZIP to a writable directory and run `Aperture.exe`. Library state, playlists, watch history, playback positions, and settings are stored in `data/` next to the executable.

### Update

Run **Settings > System & Storage > Update Aperture** or `Update.cmd`. The updater fetches the latest GitHub release, verifies the SHA-256 checksum and manifest, replaces runtime files, and preserves `data/` and custom `mpv.conf`. The last three pre-update snapshots are kept in `update-backups/`.

### Build

Requires Visual Studio 2022 (Desktop development with C++), Windows SDK, CMake, and Python 3. Bundled dependencies and checksums are tracked in `scripts/dependencies.json`.

- **Full release build, test, and package**:
  ```powershell
  ./build_and_release.ps1
  ./build_and_release.ps1 -IncludeDebug
  ```
- **Compile only**:
  ```powershell
  ./build.ps1
  ./build.ps1 -Debug
  ./build.ps1 -Clean
  ```
- **Regression suite**:
  ```powershell
  ./tests/run_tests.ps1 -Tier pr
  ```

---

## Linux (x86_64)

Native Linux backend (`src/linux/`) using SDL2, Cairo, Pango, and `libmpv` (Wayland and X11).

### Dependencies (Arch / CachyOS)

```bash
sudo pacman -S mpv sdl2 cairo pango glib2 gdk-pixbuf2 curl dbus openssl
# Optional (stream resolution, thumbnails, file dialogs):
sudo pacman -S yt-dlp ffmpeg kdialog zenity
```

### Run / Install

- **Portable archive**: Extract `dist/Aperture-<version>-linux-x86_64.tar.gz` and run `./aperture`. If a writable `./data/` directory exists beside the binary, user data is stored there; otherwise it defaults to `$XDG_DATA_HOME/aperture` (`~/.local/share/aperture`).
- **System package (`PKGBUILD`)**:
  ```bash
  cd packaging/arch
  makepkg -si
  ```
  Installs `/usr/bin/aperture`, `/usr/share/applications/aperture.desktop`, `/usr/share/aperture/fonts/`, and `/usr/share/aperture/locales/`.

### Build

- **Native (Linux CMake + Ninja)**:
  ```bash
  ./build_linux.sh
  ./build_and_release_linux.sh
  ```
- **Cross-compile from Windows (Zig C++)**:
  ```powershell
  ./build_linux.ps1
  ```

---

## Releases & Documentation

Builds, tests, and packaging run locally without GitHub Actions:

1. Set version: `./scripts/set_version.ps1 -Version <major.minor.patch.build>`
2. Commit and push source to `origin`.
3. Prepare offline notes/archive (`./scripts/publish_release.ps1 -PrepareOnly`) or publish to GitHub Releases (`./scripts/publish_release.ps1`, requires `gh auth login`).

- [Performance Checkpoint Dashboard](docs/checkpoints.html) ([guide](docs/CHECKPOINTS.md))
- [Documentation Index](docs/README.md)
