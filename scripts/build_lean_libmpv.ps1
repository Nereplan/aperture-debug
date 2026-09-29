<#
.SYNOPSIS
    Builds a lean, Aperture-tuned libmpv-2.dll on a machine with MSYS2/MinGW-w64.

.DESCRIPTION
    Aperture only plays 37 extensions (see MediaLibrary::GetSupportedExtensions),
    yet the vendored libmpv-2.dll (~47 MB) ships full FFmpeg/mpv fat measured
    in-repo: vapoursynth, dvdnav, bluray/aacs, libarchive, zimg, ANGLE/EGL,
    dxva2+nvdec alongside d3d11va, OpenCL, Lua scripts, and every FFmpeg
    decoder/demuxer/protocol/filter/muxer/encoder.

    This script drives an MSYS2 MINGW64 environment (msvcrt, matching the
    current DLL) to build FFmpeg --disable-everything + only-what-Aperture-plays,
    then mpv as libmpv-only shared, statically linked, stripped.

    MUST run on a machine with MSYS2 installed. All work happens under
    BuildRoot (default: scratch\mpv-build inside
    the app folder). Expect 30-90 minutes and 10-20 GB transient disk use.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build_lean_libmpv.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build_lean_libmpv.ps1 -MsysRoot D:\msys64 -Jobs 16
#>
param(
    [string]$MsysRoot = "C:\msys64",
    [string]$BuildRoot = (Join-Path $PSScriptRoot "..\scratch\mpv-build"),
    [string]$OutputDir = (Join-Path $PSScriptRoot "..\scratch\mpv-build\output"),
    [int]$Jobs = 0,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$OutputDir = [IO.Path]::GetFullPath($OutputDir)

$bashExe = Join-Path $MsysRoot "usr\bin\bash.exe"
if (-not (Test-Path -LiteralPath $bashExe)) {
    throw "MSYS2 bash not found at $bashExe. Install MSYS2 (https://www.msys2.org), then re-run. Nothing was built."
}

if ($Clean -and (Test-Path -LiteralPath $BuildRoot)) {
    Remove-Item -LiteralPath $BuildRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
if ($OutputDir -ne $BuildRoot -and -not $OutputDir.StartsWith($BuildRoot + [IO.Path]::DirectorySeparatorChar)) {
    throw "OutputDir must live under BuildRoot (space-free alias only covers the build tree)."
}

# Space-free alias: pkg-config tokenizes -L paths on whitespace, and meson
# inherits the fragments, so any space in $PREFIX silently truncates every
# -L$PREFIX/lib into garbage (observed: -L"C:/.../Aperture). subst maps a
# drive letter onto BuildRoot; every byte still lands physically inside the
# app folder's scratch\mpv-build. The mapping is removed in the finally block.
$driveLetter = $null
foreach ($c in @('Q', 'T', 'Y', 'X', 'Z', 'W', 'V', 'U')) {
    if (-not (Test-Path -LiteralPath "$c`:\")) { $driveLetter = $c; break }
}
if (-not $driveLetter) { throw "No free drive letter for the space-free build alias." }
subst "$driveLetter`:" /D 2>$null | Out-Null
subst "$driveLetter`:" "$BuildRoot" | Out-Null
$msysBuildRoot = "/$($driveLetter.ToLower())"

if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

# NOTE: single-quoted here-string: $ stays literal for bash.
$buildSh = @'
set -euxo pipefail

JOBS="__JOBS__"
PREFIX="$PWD/prefix"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:/mingw64/lib/pkgconfig"
export PATH="$PREFIX/bin:/mingw64/bin:/usr/bin:$PATH"
# Keep function-level dead-code elimination, but deliberately avoid
# -fdata-sections on MinGW. FFmpeg's large aligned zero-initialized TX tables
# are otherwise emitted as file-backed .data$ff_tx_tab_* sections instead of
# .bss, adding roughly 19 MB of zeroes to libmpv-2.dll.
export CFLAGS="${CFLAGS:-} -ffunction-sections"
export CXXFLAGS="${CXXFLAGS:-} -ffunction-sections"
# Static libgcc/stdc++ everywhere (meson honors LDFLAGS; ffmpeg gets the same
# pair via --extra-ldflags below). No global -static: several system deps
# (vulkan loader etc.) exist only as import libs and must resolve shared.
# winpthread is force-static-scoped: otherwise the final DLL imports
# libwinpthread-1.dll (an MSYS2 redist, NOT a system DLL — verified present in
# an early link), which would break loading on machines without MSYS2.
export LDFLAGS="-static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic"

# --- 1. Toolchain + third-party libs (binary packages, static-preferred) ---
pacman -Syuu --noconfirm || pacman -Syuu --noconfirm || true
pacman -S --needed --noconfirm \
  base-devel git \
  mingw-w64-x86_64-toolchain mingw-w64-x86_64-meson mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-nasm mingw-w64-x86_64-pkgconf mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-python \
  mingw-w64-x86_64-dav1d mingw-w64-x86_64-libplacebo mingw-w64-x86_64-shaderc \
  mingw-w64-x86_64-libass mingw-w64-x86_64-lcms2 \
  mingw-w64-x86_64-uchardet mingw-w64-x86_64-fontconfig mingw-w64-x86_64-freetype \
  mingw-w64-x86_64-harfbuzz mingw-w64-x86_64-fribidi mingw-w64-x86_64-libwebp \
  mingw-w64-x86_64-vulkan-headers mingw-w64-x86_64-zlib mingw-w64-x86_64-bzip2 \
  mingw-w64-x86_64-xz mingw-w64-x86_64-xxhash mingw-w64-x86_64-python-jinja

# --- 2. Pinned sources (match the app's client API 2.5 / ABI expectations) ---
FFMPEG_TAG="n8.0"
MPV_TAG="v0.41.0"
LIBPLACEBO_TAG="v7.360.1"
[ -d ffmpeg ] || git clone --depth 1 --branch "$FFMPEG_TAG" https://github.com/FFmpeg/FFmpeg.git ffmpeg
[ -d mpv ] || git clone --depth 1 --branch "$MPV_TAG" https://github.com/mpv-player/mpv.git mpv
[ -d libplacebo ] || git clone --depth 1 --branch "$LIBPLACEBO_TAG" https://github.com/haasn/libplacebo.git libplacebo

# --- 3. FFmpeg: disable everything, enable only what the 37 extensions need ---
cd ffmpeg
# A killed or reconfigured FFmpeg build can leave stale/zero-byte objects that
# make a later archive silently wrong. This checkout is build scratch, so start
# every configure from a clean generated state while keeping tracked sources.
git clean -fdx
./configure \
  --arch=x86_64 --target-os=mingw32 --prefix="$PREFIX" \
  --disable-debug --disable-doc --disable-htmlpages --disable-manpages --disable-podpages --disable-txtpages --disable-programs \
  --disable-avdevice --disable-devices \
  --disable-encoders --enable-encoder=png \
  --disable-muxers --enable-muxer=image2 \
  --disable-demuxers \
  --enable-demuxer=mov --enable-demuxer=matroska --enable-demuxer=avi --enable-demuxer=asf \
  --enable-demuxer=flv --enable-demuxer=mpegps --enable-demuxer=mpegts --enable-demuxer=mpegvideo \
  --enable-demuxer=ogg --enable-demuxer=rm --enable-demuxer=wav --enable-demuxer=mp3 \
  --enable-demuxer=flac --enable-demuxer=aac --enable-demuxer=ac3 --enable-demuxer=dts \
  --enable-demuxer=aiff --enable-demuxer=amr --enable-demuxer=hls \
  --enable-demuxer=rtsp --enable-demuxer=sdp \
  --enable-demuxer=ass --enable-demuxer=srt --enable-demuxer=webvtt --enable-demuxer=microdvd --enable-demuxer=mpl2 \
  --disable-decoders \
  --enable-decoder=h264 --enable-decoder=hevc \
  --enable-decoder=mpeg1video --enable-decoder=mpeg2video --enable-decoder=mpeg4 \
  --enable-decoder=msmpeg4v3 --enable-decoder=msmpeg4v2 \
  --enable-decoder=wmv1 --enable-decoder=wmv2 --enable-decoder=wmv3 --enable-decoder=vc1 \
  --enable-decoder=flv --enable-decoder=vp6 --enable-decoder=vp6f \
  --enable-decoder=theora --enable-decoder=vp8 --enable-decoder=vp9 --enable-decoder=libdav1d \
  --enable-decoder=h263 --enable-decoder=rv10 --enable-decoder=rv20 --enable-decoder=rv30 --enable-decoder=rv40 \
  --enable-decoder=mjpeg --enable-decoder=png --enable-decoder=dvvideo --enable-decoder=prores \
  --enable-decoder=huffyuv --enable-decoder=ffv1 --enable-decoder=qtrle --enable-decoder=svq1 --enable-decoder=svq3 \
  --enable-decoder=cinepak --enable-decoder=fraps --enable-decoder=msrle --enable-decoder=msvideo1 \
  --enable-decoder=bmp --enable-decoder=gif \
  --enable-decoder=mp3float --enable-decoder=aac --enable-decoder=aac_latm \
  --enable-decoder=ac3 --enable-decoder=eac3 --enable-decoder=dca \
  --enable-decoder=flac --enable-decoder=vorbis --enable-decoder=opus --enable-decoder=alac \
  --enable-decoder=wmav1 --enable-decoder=wmav2 --enable-decoder=wmavoice \
  --enable-decoder=pcm_alaw --enable-decoder=pcm_mulaw --enable-decoder=pcm_s8 --enable-decoder=pcm_s8_planar \
  --enable-decoder=pcm_s16be --enable-decoder=pcm_s16be_planar --enable-decoder=pcm_s16le --enable-decoder=pcm_s16le_planar \
  --enable-decoder=pcm_s24be --enable-decoder=pcm_s24le --enable-decoder=pcm_s24le_planar \
  --enable-decoder=pcm_s32be --enable-decoder=pcm_s32le --enable-decoder=pcm_s32le_planar \
  --enable-decoder=pcm_u8 --enable-decoder=pcm_u16le --enable-decoder=pcm_u24le --enable-decoder=pcm_u32le \
  --enable-decoder=pcm_f32le --enable-decoder=pcm_f64le \
  --enable-decoder=adpcm_ms --enable-decoder=adpcm_ima_wav --enable-decoder=adpcm_ima_qt --enable-decoder=adpcm_yamaha \
  --enable-decoder=amrnb --enable-decoder=amrwb \
  --enable-decoder=cook --enable-decoder=sipr --enable-decoder=ra_144 \
  --enable-decoder=truehd --enable-decoder=mlp \
  --enable-decoder=qdm2 --enable-decoder=mp1 --enable-decoder=mp2 --enable-decoder=nellymoser --enable-decoder=speex \
  --enable-decoder=g723_1 --enable-decoder=mace3 --enable-decoder=mace6 --enable-decoder=on2avc \
  --enable-decoder=ass --enable-decoder=subrip --enable-decoder=webvtt --enable-decoder=movtext \
  --enable-decoder=dvdsub --enable-decoder=dvbsub --enable-decoder=pgssub --enable-decoder=xsub \
  --enable-decoder=ccaption --enable-decoder=microdvd --enable-decoder=mpl2 --enable-decoder=realtext --enable-decoder=sami \
  --disable-protocols \
  --enable-protocol=file --enable-protocol=pipe --enable-protocol=http --enable-protocol=https \
  --enable-protocol=tcp --enable-protocol=tls --enable-protocol=rtmp --enable-protocol=rtp --enable-protocol=udp \
  --enable-protocol=data --enable-protocol=subfile --enable-protocol=concat --enable-protocol=crypto --enable-protocol=hls \
  --disable-filters --enable-filter=scale --enable-filter=format --enable-filter=fps --enable-filter=aformat --enable-filter=aresample \
  --disable-bsfs \
  --enable-bsf=aac_adtstoasc --enable-bsf=h264_mp4toannexb --enable-bsf=hevc_mp4toannexb \
  --enable-bsf=mpeg4_unpack_bframes --enable-bsf=vp9_superframe --enable-bsf=av1_frame_merge \
  --enable-bsf=extract_extradata \
  --enable-libdav1d --enable-lcms2 \
  --enable-zlib --enable-bzlib --enable-lzma \
  --disable-sdl2 --disable-opencl --disable-vaapi --disable-vdpau --disable-libdrm \
  --disable-d3d12va --disable-dxva2 --disable-vulkan \
  --disable-libxcb --disable-xlib \
  --disable-hwaccels \
  --enable-hwaccel=h264_d3d11va --enable-hwaccel=h264_d3d11va2 \
  --enable-hwaccel=hevc_d3d11va --enable-hwaccel=hevc_d3d11va2 \
  --enable-hwaccel=mpeg2_d3d11va --enable-hwaccel=mpeg2_d3d11va2 \
  --enable-hwaccel=vc1_d3d11va --enable-hwaccel=vc1_d3d11va2 \
  --enable-hwaccel=wmv3_d3d11va --enable-hwaccel=wmv3_d3d11va2 \
  --enable-hwaccel=vp9_d3d11va --enable-hwaccel=vp9_d3d11va2 \
  --disable-shared --enable-static --enable-pic \
  --extra-cflags="-ffunction-sections" \
  --extra-ldflags="-static-libgcc -static-libstdc++ -Wl,--gc-sections"
make -j"$JOBS"
make install
cd ..
# Fixup: ffbuild/pkgconfig_generate.sh sources config.sh, whose unquoted
# prefix= line word-splits on the space in our path, truncating the prefix
# recorded in the installed libav*.pc/libsw*.pc files. Repair them in place
# (harmless if already correct).
for pc in "$PREFIX"/lib/pkgconfig/libav*.pc "$PREFIX"/lib/pkgconfig/libsw*.pc; do
  [ -f "$pc" ] || continue
  sed -i "s|^prefix=.*|prefix=$PREFIX|" "$pc"
done

# --- 4. Static shadows for shared-only .pc names ---
# mpv queries 'shaderc' (-> shared import lib) and 'spirv-cross-c-shared'.
# Static archives exist for both (libshaderc_combined.a, libspirv-cross-c.a),
# so shadow those two names with static content inside OUR prefix, which is
# first in PKG_CONFIG_PATH. System files are untouched. Final DLL then carries
# no shaderc/spirv-cross DLL imports.
mkdir -p "$PREFIX/lib/pkgconfig"
# shaderc_combined.pc ships no Libs.private, but libshaderc_combined.a needs
# glslang (TProgram/TShader) and SPIRV-Tools (Optimizer/Validator). All static
# archives verified present. Dependents-first order for single-pass ld.
# Both stems are shadowed: mpv queries 'shaderc', libplacebo.pc Requires names
# 'shaderc_combined' — a missing stem once cost a full link cycle (observed).
SHADERC_STATIC_LIBS="-lshaderc_combined -lglslang -lglslang-default-resource-limits -lSPIRV-Tools-opt -lSPIRV-Tools"
sed "s/-lshaderc_shared/$SHADERC_STATIC_LIBS/" /mingw64/lib/pkgconfig/shaderc.pc > "$PREFIX/lib/pkgconfig/shaderc.pc"
sed "s/-lshaderc_combined/$SHADERC_STATIC_LIBS/" /mingw64/lib/pkgconfig/shaderc_combined.pc > "$PREFIX/lib/pkgconfig/shaderc_combined.pc"
if grep -q -- '-lspirv-cross-c-shared' /mingw64/lib/pkgconfig/spirv-cross-c-shared.pc 2>/dev/null; then
  # libspirv-cross-c.a needs its sibling backends (HLSL for our D3D11 path);
  # list dependents-first for single-pass ld. All archives verified present.
  sed 's/-lspirv-cross-c-shared/-lspirv-cross-c -lspirv-cross-hlsl -lspirv-cross-glsl -lspirv-cross-msl -lspirv-cross-cpp -lspirv-cross-reflect -lspirv-cross-core -lspirv-cross-util/g' /mingw64/lib/pkgconfig/spirv-cross-c-shared.pc > "$PREFIX/lib/pkgconfig/spirv-cross-c-shared.pc"
fi
# Meson rewrites .pc -I paths to our $PREFIX (verified in meson-info:
# spirv-cross-c-shared recorded -I$PREFIX/include/spirv_cross). Vendor those
# headers into the prefix so the rewritten -I always resolves. (Top-level
# includes like shaderc/ are already covered by the -I/mingw64 system path.)
mkdir -p "$PREFIX/include/spirv_cross"
cp /mingw64/include/spirv_cross/* "$PREFIX/include/spirv_cross/" 2>/dev/null || true
# NOTE on --enable-libplacebo: intentionally ABSENT from the FFmpeg flags
# above. It only feeds FFmpeg's vf_libplacebo filter (unused: mpv renders via
# libplacebo directly), and the system libplacebo.pc drags shared-only
# shaderc/vulkan libs into FFmpeg's link check. mpv gets its own source-built
# libplacebo below instead.
# NOTE on -static: an earlier revision passed a global -static ldflag plus
# --pkg-config-flags=--static. That makes ld reject shared-only import libs
# outright (shaderc_shared, spirv-cross-c-shared, vulkan-1 have no static
# archives) and fails configure. Static-ness is decided per-dependency at the
# final mpv link via --prefer-static + the shadows above instead.

# --- 5. libplacebo from source: D3D11-only, no Vulkan ---
# Rationale: the shipped DLL provably links no vulkan-1 (byte-verified), and a
# hard vulkan-1.dll import would make libmpv unloadable wherever the staged
# loader is absent. System libplacebo is vulkan-enabled, so build our own.
# Option names verified against libplacebo v7.360.1 meson_options.txt.
cd libplacebo
PLACEBO_OPTS=(--prefix="$PREFIX" --buildtype=release --prefer-static -Db_lto=false -Db_ndebug=true -Ddefault_library=static -Dvulkan=disabled -Dd3d11=enabled -Dopengl=disabled -Dshaderc=enabled -Dglslang=disabled -Dlcms=enabled -Ddemos=false -Dtests=false -Dbench=false -Dfuzz=false)
# Always start clean: meson caches pkg-config detection, and our shadow .pc
# files evolve between runs (a stale cache silently relinked old flags once).
rm -rf build
meson setup build "${PLACEBO_OPTS[@]}"
ninja -C build -j"$JOBS"
meson install -C build
# libplacebo writes an unconditional 'vulkan' into Requires even when built
# -Dvulkan=disabled (pl_has_vulkan=0, verified in the installed .pc). A hard
# -lvulkan-1 would make libmpv unloadable without the staged loader, which the
# shipped DLL provably avoids — strip it. Safe: no vulkan symbols exist in a
# vulkan-disabled archive, so a successful final link proves completeness.
sed -i 's/, vulkan\([, ]\)/\1/' "$PREFIX/lib/pkgconfig/libplacebo.pc"
grep -E '^Requires' "$PREFIX/lib/pkgconfig/libplacebo.pc"
cd ..

# --- 6. mpv: libmpv-only shared, lean features ---
# Option names verified against mpv v0.41.0 meson.options (which uses
# 'meson.options', NOT 'meson_options.txt'). Adjustments vs the first draft:
# removed -Dlibavdevice/-Dencoding/-Dsdl2 (no such options; sdl2-* and
# libavdevice coverage come from FFmpeg --disable-avdevice/--disable-devices
# plus sdl2-* defaults-off); removed -Dlibplacebo (hard dependency, not an
# option); added determinism/discipline flags below.
cd mpv
# Bash array (not a flat string): BuildRoot contains spaces, so word splitting
# must preserve the --prefix value as one argument.
# Every -D name below was verified against mpv v0.41.0 meson.options.
MESON_OPTS=(--prefix="$PREFIX" --buildtype=release --prefer-static -Db_lto=false -Db_ndebug=true -Dstrip=true -Dbuild-date=false -Dlibmpv=true -Dcplayer=false -Dtests=false -Dmanpage-build=disabled -Dlua=disabled -Djavascript=disabled -Dlibarchive=disabled -Ddvdnav=disabled -Dlibbluray=disabled -Dvapoursynth=disabled -Dvulkan=disabled -Dshaderc=enabled -Dspirv-cross=enabled -Dd3d11=enabled -Ddirect3d=disabled -Dd3d-hwaccel=enabled -Dd3d9-hwaccel=disabled -Duchardet=enabled -Drubberband=disabled -Dlcms2=enabled -Dcdda=disabled -Dzimg=disabled -Djpeg=disabled -Dwasapi=enabled -Dgl=disabled -Dplain-gl=disabled -Dgl-win32=disabled -Dgl-dxinterop=disabled -Dgl-dxinterop-d3d9=disabled -Degl=disabled -Degl-angle=disabled -Degl-angle-lib=disabled -Degl-angle-win32=disabled -Degl-drm=disabled -Degl-wayland=disabled -Degl-x11=disabled -Degl-android=disabled -Dcaca=disabled -Dsixel=disabled -Ddrm=disabled -Dvaapi=disabled -Dvdpau=disabled -Dvdpau-gl-x11=disabled -Dvaapi-drm=disabled -Dvaapi-wayland=disabled -Dvaapi-x11=disabled -Dvaapi-win32=disabled -Dxv=disabled -Dx11=disabled -Dwayland=disabled -Dcocoa=disabled -Dcuda-hwaccel=disabled -Dcuda-interop=disabled -Dalsa=disabled -Djack=disabled -Doss-audio=disabled -Dpipewire=disabled -Dpulse=disabled -Dsndio=disabled -Ddvbin=disabled)
# Always start clean (same stale-cache rationale as libplacebo above).
rm -rf build
meson setup build "${MESON_OPTS[@]}"
# Recorded adjustments (verified 2026-09-03 against mpv v0.41.0 / FFmpeg n8.0):
# - FFmpeg: dropped --disable-postproc (removed upstream in 8.0),
#   --enable-libplacebo (only feeds the unused vf_libplacebo filter; its .pc
#   drags shared-only libs into the link check), and the global -static
#   ldflag + --pkg-config-flags=--static (ld rejects shared-only import libs
#   outright). Added --disable-hwaccels/--enable-hwaccel=d3d11va (drops native
#   nvdec/dxva2 code) and kept iconv enabled (subtitle charsets).
# - mpv: dropped -Dlibavdevice/-Dencoding/-Dsdl2 (options do not exist in
#   v0.41.0) and -Dlibplacebo (hard dependency, not an option).
# - Vulkan is disabled at BOTH layers (mpv -Dvulkan=disabled, libplacebo
#   source-built -Dvulkan=disabled): the tuned DLL has no vulkan-1 import, so
#   the portable package has no Vulkan loader dependency.
# - LDFLAGS scope winpthread static (-Wl,-Bstatic -lwinpthread -Wl,-Bdynamic):
#   without it the link imports libwinpthread-1.dll (an MSYS2 redist, NOT a
#   system DLL — caught by the DLL Name check), breaking load on machines
#   without MSYS2. Observed, fixed, verified absent.
# - libplacebo is source-built (v7.360.1, meson options read from its own
#   meson_options.txt) because the system one is vulkan-enabled.
# - shaderc/spirv-cross resolve via static shadows in $PREFIX (see step 4):
#   shaderc.pc AND shaderc_combined.pc both carry the glslang + SPIRV-Tools
#   closure (mpv queries 'shaderc', libplacebo.pc Requires 'shaderc_combined'
#   — a missing stem cost a full link cycle), plus the spirv-cross sibling
#   cluster. Meson rewrites .pc -I paths to $PREFIX, so spirv_cross headers
#   are vendored there too.
# - BuildRoot MUST stay behind the subst drive alias: pkg-config tokenizes -L
#   on whitespace, silently truncating spaced paths (observed as -L".../Aperture
#   in the link response file).
# - Stray 0-byte .o files after a killed run break archives opaquely; the
#   script does not auto-purge them (delete manually if a run is ever killed).
ninja -C build -j"$JOBS"
DESTDIR="$PWD/stage" ninja -C build install
DLL=$(find stage -name 'libmpv-2.dll' | head -n 1)
test -n "$DLL"
strip --strip-unneeded "$DLL" || true
cp "$DLL" "__OUTPUT__/libmpv-2.dll"

# --- 7. Verify: arch, self-containment, exports, size, hash ---
# (objdump: native name inside the MINGW64 env; the prefixed name does not exist there.)
objdump -f "__OUTPUT__/libmpv-2.dll" | grep -i 'x86-64' || (echo 'ARCH CHECK FAILED'; exit 1)
echo '--- linked DLLs (expect system + msvcrt only) ---'
DUMP_FILE="__OUTPUT__/libmpv-objdump.txt"
objdump -p "__OUTPUT__/libmpv-2.dll" > "$DUMP_FILE"
grep 'DLL Name' "$DUMP_FILE"
echo '--- mpv API exports ---'
for sym in \
  mpv_client_api_version mpv_create mpv_initialize mpv_destroy mpv_terminate_destroy \
  mpv_command mpv_command_string mpv_set_option mpv_set_option_string \
  mpv_get_property mpv_get_property_string mpv_set_property mpv_set_property_string \
  mpv_free mpv_observe_property mpv_wait_event mpv_error_string \
  mpv_render_context_create mpv_render_context_set_update_callback \
  mpv_render_context_update mpv_render_context_render mpv_render_context_free; do
  grep -Fq "$sym" "$DUMP_FILE" || { echo "EXPORT CHECK FAILED: $sym"; exit 1; }
done
IMPORTS=$(grep 'DLL Name' "$DUMP_FILE" || true)
if printf '%s\n' "$IMPORTS" | grep -Eqi 'libgcc|libstdc\+\+|libwinpthread|libshaderc|spirv-cross|vulkan-1'; then
  echo 'SELF-CONTAINMENT CHECK FAILED: non-system runtime dependency detected'
  printf '%s\n' "$IMPORTS"
  exit 1
fi
rm -f "$DUMP_FILE"
ls -l "__OUTPUT__/libmpv-2.dll"
sha256sum "__OUTPUT__/libmpv-2.dll"
echo 'BUILD OK. Next: swap into a package copy, update scripts/dependencies.json (libmpv sha256/size), then run scripts\verify_package.ps1, tests\corpus\run_tests.ps1, scripts\run_benchmark.ps1, and the playback smoke harness.'
'@
$buildSh = $buildSh.Replace('__JOBS__', "$Jobs")
$buildSh = $buildSh.Replace('__OUTPUT__', "$msysBuildRoot/output")

$shPath = Join-Path $BuildRoot "build.sh"
Set-Content -LiteralPath $shPath -Value $buildSh -Encoding Ascii -NoNewline:$false

Write-Host "Running lean libmpv build under MSYS2 MINGW64 (this takes 30-90 minutes)..." -ForegroundColor Yellow
Write-Host "Build alias: $driveLetter`: = $BuildRoot (space-free path for pkg-config)" -ForegroundColor DarkGray
# MINGW64/MSVCRT (not UCRT64): matches the shipped DLL's msvcrt linkage.
# Must be set in our environment so it is inherited by bash.exe.
$env:MSYSTEM = 'MINGW64'
try {
    & $bashExe -lc "cd '$msysBuildRoot' && exec bash ./build.sh"
    if ($LASTEXITCODE -ne 0) { throw "Lean libmpv build failed with exit code $LASTEXITCODE. See output above." }
}
finally {
    subst "$driveLetter`:" /D 2>$null | Out-Null
}

$dll = Join-Path $OutputDir "libmpv-2.dll"
$info = Get-Item -LiteralPath $dll
Write-Host ("`nSUCCESS! Lean DLL: {0} ({1:N0} bytes, was 47,732,224)" -f $dll, $info.Length) -ForegroundColor Green
Write-Host ("SHA-256: {0}" -f (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash)
