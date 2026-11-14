# Laptop (AMD Ryzen AI 7 350 / Radeon 860M) Full Benchmark Evidence (2026-11-14)

- Commit: `7e3a6833557ddac583fa1878281906d2e95c5bd5` (`clean`, prior to evidence archival)
- Machine: `AMD Ryzen AI 7 350 w/ Radeon 860M` (8 cores / 16 logical processors), `AMD Radeon(TM) 860M Graphics` (driver `32.0.32015.7029`), Windows 11 Home `10.0.26200 build 26200`, ~23.66 GiB RAM, 120 DPI (125% scale)
- Packaged `Aperture.exe` SHA-256: `0142540bde580b02e1b86ffa03365d3297ddfe8210cf35993f283c635c72fcd5`
- Official `vlc-3.0.24-win64.7z` SHA-256: `1ed59c09152e78aff84663fa76efab26057716576db74332858810e5ef54ae1f`
- Invariant-TSC calibration (`cpu-calibration.json`): `1996158436.191914` Hz (`spread=0.000002296`)

## Measured Medians
- **Empty portable startup (`startup-report.json`, 5 samples after warmups)**: shell `32.05 ms`, window shown `24.18 ms`, populated `85.7 ms`, WS `53.25 MiB`, Private `41.59 MiB`
- **Visible HubThenOpen 1080p30 H.264 playback (`rival-h264/comparison.json`, 1+3 runs, 30s window at 1280x720 logical / 120 DPI)**:
  - Aperture vs VLC 3.0.24: visible window `76.21 ms` vs `499.32 ms`, private RAM `164.66 MiB` vs `357.61 MiB`, working set `130.22 MiB` vs `152.37 MiB`, dedicated GPU `118.04 MiB` vs `233.40 MiB`, TSC CPU `5.438%` vs `5.799%` (Windows CPU-time `5.779%` vs `4.685%`)
  - Qualification note: `evaluate_rival.py` exited 2 (`Sampled canvas changed`) because `screenCanvas` reports physical desktop pixels at 120 DPI (`1601x901` Aperture / `1601x900` VLC) while `videoCanvasTarget` is logical `[1280, 720]`; all 8 rounds reported `advancing-playback-verified` with `clientWidth=1280, clientHeight=720` and zero occlusion/resize.
- **Playback progress capture (`capture-run1..3.csv`, 21 samples each)**: 500 unchanged `0.0060 ms` (0 allocs); 500 change `0.0080 ms` (`4,544` B); 25k unchanged `0.0090 ms` (0 allocs); 25k change `0.0160 ms` (`7,616` B).
- **Post-audit microbenchmarks (`audit-run1..3.csv`, 21 samples each)**: empty 4K overlay `0.0000 ms` (vs `110.5011 ms`), 4K vlc-outline `5.5742 ms` (vs `5.6745 ms`), RSS 20x100 `7.1630 ms` (vs `23.3510 ms`), 10k HubExtract lookups `0.1602 ms` (vs `19.1014 ms`).
