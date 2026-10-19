#!/bin/bash
set -euo pipefail
export PATH="/mingw64/bin:/usr/bin:$PATH"
prefix="$1"
source_file=$(cygpath -u "$2")
output_file=$(cygpath -u "$3")
case "$4" in
  size) optimization=-Os ;;
  balanced) optimization=-O2 ;;
  performance) optimization=-O3 ;;
  *) echo 'Unknown optimization profile' >&2; exit 2 ;;
esac
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:/mingw64/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
# A static executable also avoids GCC's implicit -pthread selecting the DLL
# after the explicit dependency list. Windows system imports remain dynamic.
gcc -std=c11 -D_WIN32_WINNT=0x0601 "$optimization" -flto=4 -fno-fast-math \
  -ffunction-sections -static "$source_file" -Wl,--gc-sections \
  $(pkgconf --define-variable="prefix=$prefix" --define-variable="libdir=$prefix/lib" \
      --define-variable="includedir=$prefix/include" --cflags --libs --static \
      libavformat libavcodec libavutil) -o "$output_file"
objdump -p "$output_file" | sed -n '/DLL Name:/p'
