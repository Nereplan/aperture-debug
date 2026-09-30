# Build and release

## Local workflow

- `Build Release.cmd`: build Release, run the PR regression gate, package a clean ZIP, then update `release/` and `debug/` without replacing their profiles.
- `Update Source.cmd`: require a clean Git checkout, run `git pull --ff-only`, then perform the same build and deployment.
- `build_and_release.ps1 -IncludeDebug`: additionally compile Debug with symbols for `debug/`.
- `build.ps1`: compile only. The default output is `debug/Aperture.Release.exe`; `-Debug` produces `debug/Aperture.exe`. It does not deploy runtime dependencies. Normal builds reuse a configuration-specific cache in `scratch/build/`; `-Clean`, static analysis and sanitizer builds use isolated temporary directories.

Required tools: Windows x64, Visual Studio 2022 C++ tools and Windows SDK, CMake, and Python 3. Dependencies are checked against `scripts/dependencies.json` before building.

The release ZIP contains runtime assets, the updater, repository/version configuration, and a SHA-256 file manifest. It never takes files from an existing application's data directory. Distribute the ZIP in `dist/`, not your working `release/` folder, which may contain personal data and backups after you use it.

## First GitHub release

1. Create a public GitHub repository and push this folder to `main`. No account name is hardcoded. GitHub sets the repository in packaged releases automatically.
2. Run `./scripts/set_version.ps1 -Version 2.0.1.0` (using your next version) to keep every field in `version.json` consistent. The display version omits a zero build number.
3. Commit and push, then run **Actions > Release > Run workflow**, or push a matching version tag (`v2.0.0.0` for the initial version).
4. Download the published ZIP for the first installation. Future releases must increase the numeric version.

The Release workflow builds and tests before creating a draft with its ZIP attached. It publishes only after the upload succeeds. Existing releases are not overwritten. An interrupted publication can leave a draft for review in GitHub. Enable GitHub immutable releases in repository settings if desired.

The portable updater supports public repositories. It does not store GitHub access tokens or signing secrets. HTTPS and [GitHub's SHA-256 asset digest](https://docs.github.com/en/rest/releases/releases) establish package integrity; the manifest then validates every extracted runtime file. This does not provide Authenticode signing. The optional signing utilities remain available for a separately provisioned signing certificate.

## Updates and saved data

`Update.cmd` or the app's **Update Aperture** button performs the same update:

1. Check the latest stable GitHub release; reject older/equal versions, unsupported tags, missing checksums, and oversized packages.
2. Download and verify the ZIP before touching the installed copy. Extract only approved runtime paths and validate all file hashes.
3. Ask running Aperture instances in this installation to close normally, allowing pending saves to finish. Stop if an instance cannot close.
4. Snapshot `data/`, `mpv.conf`, and the runtime files being replaced under `update-backups/`.
5. Replace runtime files individually, restoring changed files if replacement fails. Leave caches, media files, personal files, and the live profile in place.
6. Retain the newest three completed snapshots and relaunch Aperture. Incomplete attempts remain available for recovery.

Snapshots include watched folders, playlists, bookmarks, history, settings, session state, and existing rotating JSON backups because the entire `data/` tree is copied. Updates never delete media referenced by the library. New application versions should keep the existing persistence migrations backward compatible and add regression coverage for any schema changes.

## Recovery

Close Aperture before restoring a snapshot. Each `update-backups/<timestamp>/runtime/` folder contains the previous versions of replaced files; copy those files back into the app directory. `journal.json` identifies files newly added by that update (`existed: false`), which can be removed when rolling back. Restore the snapshot's `data/` and `mpv.conf` only if you also need the pre-update profile; doing so discards changes made since the update.

A power interruption may leave a partially replaced runtime. The backup and journal are written before replacement begins, so the same recovery steps apply. Runtime replacement failures are rolled back automatically while the updater is running. Backup cleanup failures do not remove recovery copies.

## Verification

`tests/package/test_update_retention.ps1` invokes the actual installer with isolated fixtures, verifies profile bytes and backup retention, causes a real file-lock failure to test rollback, and rejects tampered packages and protected paths. It is part of the PR test gate alongside persistence, updater, library, unit, and resource tests.
