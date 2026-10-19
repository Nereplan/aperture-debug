# Sourced before fetching or building anything. Keep all profiles portable;
# neither changes CPU requirements, floating-point semantics or decoder features.
NATIVE_PROFILE="${APERTURE_NATIVE_PROFILE:-size}"
case "$NATIVE_PROFILE" in
  size)
    FFMPEG_SMALL_FLAG=--enable-small
    NATIVE_OPT_FLAG=-Os
    MESON_OPT_LEVEL=s
    ;;
  balanced)
    FFMPEG_SMALL_FLAG=--disable-small
    NATIVE_OPT_FLAG=-O2
    MESON_OPT_LEVEL=2
    ;;
  performance)
    FFMPEG_SMALL_FLAG=--disable-small
    NATIVE_OPT_FLAG=-O3
    MESON_OPT_LEVEL=3
    ;;
  *)
    echo "Unknown native optimization profile: $NATIVE_PROFILE" >&2
    return 2
    ;;
esac
