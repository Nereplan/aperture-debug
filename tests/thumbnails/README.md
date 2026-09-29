# Thumbnail storage and behavior tests

`tests/thumbnails/benchmark_lookup.ps1` separately measures a synthetic model of
the renderer's bitmap ownership lookup before and after pointer indexing. It
checks matching results for the most recent thumbnail, other cached thumbnails,
and external preview misses in a 291-entry cache. Timings exclude Direct2D and
do not measure whole-app frame rate. The unit suite uses actual Direct2D bitmaps
to verify index admission, rounded drawing, eviction, and cache clearing.

The PR gate runs `tests/thumbnails/run_tests.ps1`. It compiles the production
codec, migration worker, and storage code in an isolated temporary directory.
It requires MSVC and Windows; it does not need mpv, media files, a desktop app,
or a personal library. It can run while another Aperture installation is open.

Covered behaviors:

- Opaque JPEG compression, lossless transparency, legacy raw/PNG reads.
- Decoded dimensions, timestamp preservation, malformed headers, truncated
  payloads, corrupt image data, and payload/header dimension disagreement.
- Background conversion, successful reload, no second JPEG recompression,
  locked-file preservation and retry, stale replacement rejection, and no
  resurrection after cache clearing.
- Library thumbnail survival during repeated seek-preview writes, independent
  tier limits, total-budget pressure, disabled storage, replacement size deltas,
  Windows path aliases, cross-class/path traversal rejection, and concurrent writers.
- Full accounting and eviction with more than 8,192 files.

The unit suite additionally exercises the actual player writer default arguments
against disabled global settings, and verifies that a private-mode read does
not migrate an existing file.

## Real thumbnail corpus

Copy the cache files to an isolated test directory first. Do not launch or point
the test application at the active installation's profile. The corpus reader is
read-only and all conversion happens beneath the explicitly selected output.

```powershell
./tests/thumbnails/run_tests.ps1 -Corpus ./debug/thumbnail-lab/original `
    -OutputRoot ./debug/thumbnail-lab/candidate
```

The corpus mode decodes every original through the production codec, encodes and
decodes candidates, reports RGB PSNR and latency, copies legacy files to the
output, migrates them through the actual bounded background worker, checks exact
file counts/byte totals, then reloads every saved image in a second pass.
Smaller original PNGs are retained. No original video is opened.

Outputs: `codec.csv`, `summary.json`, and `cache/thumbnails/*.bin`.
The output must be inside this workspace, separate from the source.

Optional JPEG/WebP quality and timing comparison (Python, Pillow with WebP, numpy):

```powershell
python ./tests/thumbnails/compare_formats.py ./debug/thumbnail-lab/original `
    ./debug/thumbnail-lab/candidate
```

This adds `formats.csv`, `formats-summary.json`, and a self-contained local
`comparison.html`. It reports 11x11 uniform-window luminance SSIM and RGB PSNR,
and tests WebP at qualities 75/80/85, method 4. Pillow JPEG/WebP timings provide a
same-library comparison; they are separate from the production WIC timings.
Quality numbers on different encoders are not assumed to be equivalent.

Storage-only before/after comparison, using an explicitly supplied old header:

```powershell
./tests/thumbnails/benchmark_storage.ps1 `
    -Corpus ./debug/thumbnail-lab/candidate/cache/thumbnails `
    -BaselineHeaderDirectory ./scratch/thumbnail-before
```

This writes and verifies identical compressed bytes using each implementation,
alternates execution order across three iterations, and removes its temporary
builds and caches. Results go to `diagnostics/thumbnail-storage-benchmark.json`.

Image metrics and codec/storage timings do not measure whole-window scrolling
or certify that every visual difference is imperceptible. Inspect the worst
images in the comparison and measure a real UI separately when needed. The
corpus contains private images and must not be committed or uploaded. Keep only
the small reports and the optimized debug cache after local measurement.
