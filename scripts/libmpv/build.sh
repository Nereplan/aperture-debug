#!/bin/bash
# Aperture x64/MSVCRT libmpv recipe. All output belongs to the wrapper's build tree.
set -euo pipefail
source recipe/build_profile.sh
export PATH="/mingw64/bin:/usr/bin:$PATH"
for tool in gcc g++ meson ninja nasm pkgconf cmake python make objdump strip curl patch; do
  command -v "$tool" >/dev/null || { echo "Missing MSYS2 MINGW64 tool: $tool"; exit 1; }
done
python recipe/fetch_sources.py
JOBS="${JOBS:-12}"
SRC="$PWD/src"
MPV_SRC="$SRC/mpv"
PREFIX="$PWD/prefix"
mkdir -p "$PREFIX" output
pacman -Q > output/msys2-packages.txt
RECIPE_ID=$({ cat recipe/* output/msys2-packages.txt; printf '%s\n' "$NATIVE_PROFILE"; } | sha256sum | cut -d' ' -f1)
if [ -f "$PREFIX/.recipe-id" ] && [ "$(cat "$PREFIX/.recipe-id")" != "$RECIPE_ID" ]; then
  echo "Build inputs changed; run the PowerShell wrapper with -Clean."; exit 1
fi
printf '%s\n' "$NATIVE_PROFILE" > output/optimization-profile.txt
printf '%s\n' "$RECIPE_ID" > "$PREFIX/.recipe-id"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:/mingw64/lib/pkgconfig"
export CFLAGS="-ffunction-sections"
export CXXFLAGS="-ffunction-sections"
# Avoid -fdata-sections: MinGW turns FFmpeg's ~19 MB zero TX tables into file data.
export LDFLAGS="-static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic"
# Preserve assembly and D3D11 acceleration. GCC 16.2 ICEs with shader-chain LTO,
# so only mpv, FFmpeg and libplacebo use LTO; shader libraries use -Os.
REL_FLAGS="-Os -ffunction-sections -DNDEBUG"
if [ ! -f "$PREFIX/.shader-done" ]; then
# --- shaderc (+ glslang + SPIRV-Tools as third_party) ---
cmake -G Ninja -S "$PWD/shader-src/shaderc" -B build-shaderc \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS_RELEASE="$REL_FLAGS" \
  -DCMAKE_CXX_FLAGS_RELEASE="$REL_FLAGS" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DBUILD_SHARED_LIBS=OFF \
  -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
  -DENABLE_GLSLANG_BINARIES=OFF -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON \
  -DENABLE_SPVREMAPPER=OFF -DENABLERTSL=OFF -DSHADERC_ENABLE_WERROR_COMPILE=OFF
ninja -C build-shaderc -j"$JOBS"
cmake --install build-shaderc

# --- SPIRV-Cross ---
cmake -G Ninja -S "$PWD/shader-src/spirv-cross" -B build-spirv-cross \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS_RELEASE="$REL_FLAGS" \
  -DCMAKE_CXX_FLAGS_RELEASE="$REL_FLAGS" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON \
  -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF \
  -DSPIRV_CROSS_ENABLE_GLSL=ON -DSPIRV_CROSS_ENABLE_HLSL=ON \
  -DSPIRV_CROSS_ENABLE_MSL=OFF -DSPIRV_CROSS_ENABLE_CPP=OFF \
  -DSPIRV_CROSS_ENABLE_REFLECT=OFF -DSPIRV_CROSS_ENABLE_UTIL=OFF
ninja -C build-spirv-cross -j"$JOBS"
cmake --install build-spirv-cross


touch "$PREFIX/.shader-done"
fi
# Use built static libraries and headers, rather than installed shared metadata.
python recipe/static_pkgconfig.py "$PREFIX"
if [ ! -f "$PREFIX/.ffmpeg-done" ]; then
# hqdn3d and stereo3d are intentionally absent from the filter allowlist: this
# is an LGPL build and configure silently disables GPL-only filters.
mkdir -p ffmpeg-build
cd ffmpeg-build
"$SRC/ffmpeg/configure" \
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
  --enable-protocol=mmsh --enable-protocol=mmst --enable-protocol=ftp \
  --disable-filters --enable-filter=scale --enable-filter=format --enable-filter=fps --enable-filter=aformat --enable-filter=aresample \
  --enable-filter=dynaudnorm --enable-filter=pan \
  --enable-filter=equalizer --enable-filter=superequalizer --enable-filter=acompressor --enable-filter=loudnorm \
  --enable-filter=bwdif --enable-filter=yadif \
  --enable-filter=unsharp --enable-filter=gradfun --enable-filter=crop \
  --enable-filter=showspectrum --enable-filter=showwaves --enable-filter=showfreqs --enable-filter=avectorscope \
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
  --enable-hwaccel=av1_d3d11va --enable-hwaccel=av1_d3d11va2 \
  --disable-shared --enable-static --enable-pic \
  --enable-lto "$FFMPEG_SMALL_FLAG" --optflags="$NATIVE_OPT_FLAG" \
  --extra-cflags="-ffunction-sections" \
  --extra-ldflags="-static-libgcc -static-libstdc++ -Wl,--gc-sections"

make -j"$JOBS"
make install
cd ..
touch "$PREFIX/.ffmpeg-done"
fi
if [ ! -f "$PREFIX/.placebo-done" ]; then
meson setup "$SRC/libplacebo" placebo-build \
  --prefix="$PREFIX" --buildtype=release --prefer-static \
  -Db_lto=true -Doptimization="$MESON_OPT_LEVEL" -Db_ndebug=true -Ddefault_library=static \
  -Dvulkan=disabled -Dd3d11=enabled -Dopengl=disabled \
  -Dshaderc=enabled -Dglslang=disabled -Dlcms=enabled -Dlibdovi=disabled \
  -Ddemos=false -Dtests=false -Dbench=false -Dfuzz=false
touch placebo-build/build.ninja
ninja -C placebo-build -j"$JOBS"
meson install -C placebo-build
# Upstream's generated pc file lists Vulkan even in a D3D11-only build.
sed -i 's/, vulkan\([, ]\)/\1/' "$PREFIX/lib/pkgconfig/libplacebo.pc"
touch "$PREFIX/.placebo-done"
fi
export LDFLAGS="$LDFLAGS $NATIVE_OPT_FLAG -Wl,-Map,mpv-link.map"
if [ ! -f mpv-build/build.ninja ]; then
meson setup "$MPV_SRC" mpv-build \
  --prefix="$PREFIX" --buildtype=release --prefer-static \
  -Db_lto=true -Doptimization="$MESON_OPT_LEVEL" \
  -Db_ndebug=true -Dstrip=true -Dbuild-date=false -Dlibmpv=true -Dcplayer=false \
  -Dtests=false -Dmanpage-build=disabled -Dlua=disabled -Djavascript=disabled \
  -Dlibarchive=disabled -Ddvdnav=disabled -Dlibbluray=disabled -Dvapoursynth=disabled \
  -Dvulkan=disabled -Dshaderc=enabled -Dspirv-cross=enabled -Dd3d11=enabled \
  -Ddirect3d=disabled -Dd3d-hwaccel=enabled -Dd3d9-hwaccel=disabled \
  -Duchardet=enabled -Drubberband=disabled -Dlcms2=enabled -Dcdda=disabled \
  -Dzimg=disabled -Djpeg=disabled -Dwasapi=enabled \
  -Dgl=disabled -Dplain-gl=disabled -Dgl-win32=disabled -Dgl-dxinterop=disabled \
  -Dgl-dxinterop-d3d9=disabled -Degl=disabled -Degl-angle=disabled -Degl-angle-lib=disabled \
  -Degl-angle-win32=disabled -Degl-drm=disabled -Degl-wayland=disabled -Degl-x11=disabled \
  -Degl-android=disabled -Dcaca=disabled -Dsixel=disabled -Ddrm=disabled -Dvaapi=disabled \
  -Dvdpau=disabled -Dvdpau-gl-x11=disabled -Dvaapi-drm=disabled -Dvaapi-wayland=disabled \
  -Dvaapi-x11=disabled -Dvaapi-win32=disabled -Dxv=disabled -Dx11=disabled -Dwayland=disabled \
  -Dcocoa=disabled -Dcuda-hwaccel=disabled -Dcuda-interop=disabled -Dalsa=disabled \
  -Djack=disabled -Doss-audio=disabled -Dpipewire=disabled -Dpulse=disabled -Dsndio=disabled \
  -Ddvbin=disabled

fi
touch mpv-build/build.ninja
ninja -C mpv-build -j"$JOBS"
DESTDIR="$PWD/stage" ninja -C mpv-build install
DLL=$(find stage -name 'libmpv-2.dll' -print -quit)
test -n "$DLL"
cp mpv-build/libmpv-2.dll output/libmpv-2.unstripped.dll
strip --strip-unneeded "$DLL"
cp "$DLL" output/libmpv-2.dll
objdump -f output/libmpv-2.dll | grep -qi x86-64
objdump -p output/libmpv-2.dll > output/pe.txt
python recipe/provenance.py
sha256sum output/libmpv-2.dll
echo "libmpv build and dependency checks passed."
