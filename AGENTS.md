# Build and release policy

- Build, test, and package Aperture locally. Do not add GitHub Actions workflows or use GitHub-hosted or self-hosted Actions runners.
- Keep meaningful source changes in descriptive Git commits. Releases use increasing four-part versions and include the changes since the previous published release.
- For future releases, set the version with `scripts/set_version.ps1`, commit and push the source, then run `scripts/publish_release.ps1` locally. It builds and runs the regression gate before uploading to GitHub Releases.
- Use `scripts/publish_release.ps1 -PrepareOnly` for local preparation without uploading. Ordinary builds use `build_and_release.ps1`.
- Never overwrite existing release tags or assets. Do not publish a release for the commit that introduced this local release workflow.

# Performance checkpoint record

- Use `docs/checkpoints.html` as the primary performance comparison and checkpoint record. Read its Agent guide or `docs/CHECKPOINTS.md` before recording measurements.
- Append measured values, dates/times, commit/dirty identity, protocols, evidence and validation with `scripts/checkpoints.py`; run its `--check` and `tests/benchmark/test_checkpoints.py` after updates.
- Keep raw evidence under `docs/evidence/`. Record future optimization findings in checkpoint notes rather than creating another performance report. Preserve the historical reports/evidence; never invent missing values or reuse an older VLC session as a fresh measurement.
