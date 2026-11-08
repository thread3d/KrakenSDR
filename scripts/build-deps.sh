#!/bin/bash
# build-deps.sh -- build ogn-rf's missing dependencies into a local prefix, no root.
#
# Why this exists: on the Xavier, `librtlsdr-dev` and `libconfig-dev` are absent
# and sudo needs a password, so `apt install` is not available. On top of that the
# packaged librtlsdr 0.6.0 is missing two functions HeIMDALL needs
# (rtlsdr_set_dithering, rtlsdr_set_sample_freq_correction_f), so the krakenrf
# fork has to be built from source anyway.
#
# Usage: ./build-deps.sh [prefix]        default: ../deps/prefix  (next to the repo)
#
# Then build ogn-rf with:
#   make CFLAGS="-I$PREFIX/include" \
#        LIBS="-L$PREFIX/lib -Wl,-rpath,$PREFIX/lib -lpthread -lm -ljpeg -lconfig -lrt"
#
# Exit 0 on success. Prints the flags to use.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PREFIX="${1:-$(cd "$SCRIPT_DIR/.." && pwd)/deps/prefix}"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)/deps/src"
JOBS="$(nproc 2>/dev/null || echo 4)"

LOGDIR="$(cd "$SCRIPT_DIR/.." && pwd)/deps/logs"

ok()   { printf '\033[32m%s\033[0m\n' "$*"; }
bad()  { printf '\033[31m%s\033[0m\n' "$*"; }
head1() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }

mkdir -p "$PREFIX" "$SRC_DIR" "$LOGDIR" || { bad "cannot create $PREFIX"; exit 1; }

head1 "target prefix"
echo "  prefix : $PREFIX"
echo "  sources: $SRC_DIR"
echo "  logs   : $LOGDIR"

# ---------------------------------------------------------------- librtlsdr
# krakenrf fork: adds the coherent multi-dongle support and the dithering /
# sample-freq-correction hooks that heimdall's rtl_daq.c calls.
head1 "librtlsdr (krakenrf fork)"
if [ -f "$PREFIX/lib/librtlsdr.so" ] || [ -f "$PREFIX/lib/aarch64-linux-gnu/librtlsdr.so" ]; then
  ok "already built"
else
  if [ ! -d "$SRC_DIR/librtlsdr/.git" ]; then
    git clone --depth 1 https://github.com/krakenrf/librtlsdr "$SRC_DIR/librtlsdr" \
      > "$LOGDIR/librtlsdr-clone.log" 2>&1 \
      || { bad "clone failed, see $LOGDIR/librtlsdr-clone.log"; exit 1; }
  fi
  cd "$SRC_DIR/librtlsdr" || exit 1
  mkdir -p build && cd build || exit 1
  cmake -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DDETACH_KERNEL_DRIVER=ON \
        .. > "$LOGDIR/librtlsdr-cmake.log" 2>&1 \
    || { bad "cmake failed, see $LOGDIR/librtlsdr-cmake.log"; exit 1; }
  make -j"$JOBS" > "$LOGDIR/librtlsdr-make.log" 2>&1 \
    || { bad "make failed, see $LOGDIR/librtlsdr-make.log"; exit 1; }
  make install > "$LOGDIR/librtlsdr-install.log" 2>&1 \
    || { bad "install failed, see $LOGDIR/librtlsdr-install.log"; exit 1; }
  ok "built and installed"
fi

# ----------------------------------------------------------------- libconfig
head1 "libconfig"
if [ -f "$PREFIX/lib/libconfig.so" ] || [ -f "$PREFIX/lib/aarch64-linux-gnu/libconfig.so" ] \
   || [ -f "$PREFIX/lib/libconfig.a" ]; then
  ok "already built"
else
  if [ ! -d "$SRC_DIR/libconfig/.git" ]; then
    git clone --depth 1 https://github.com/hyperrealm/libconfig "$SRC_DIR/libconfig" \
      > "$LOGDIR/libconfig-clone.log" 2>&1 \
      || { bad "clone failed, see $LOGDIR/libconfig-clone.log"; exit 1; }
  fi
  cd "$SRC_DIR/libconfig" || exit 1
  # libconfig ships autotools. autoreconf needs libtool, which is NOT installed;
  # but the release tarballs carry a configure script. Prefer CMake if present.
  if [ -f CMakeLists.txt ]; then
    mkdir -p build && cd build || exit 1
    cmake -DCMAKE_INSTALL_PREFIX="$PREFIX" \
          -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF \
          .. > "$LOGDIR/libconfig-cmake.log" 2>&1 \
      || { bad "cmake failed, see $LOGDIR/libconfig-cmake.log"; exit 1; }
    make -j"$JOBS" > "$LOGDIR/libconfig-make.log" 2>&1 \
      || { bad "make failed, see $LOGDIR/libconfig-make.log"; exit 1; }
    make install > "$LOGDIR/libconfig-install.log" 2>&1 \
      || { bad "install failed, see $LOGDIR/libconfig-install.log"; exit 1; }
    ok "built and installed (cmake)"
  elif [ -x ./configure ]; then
    ./configure --prefix="$PREFIX" --disable-examples --disable-tests \
      > "$LOGDIR/libconfig-configure.log" 2>&1 \
      || { bad "configure failed, see $LOGDIR/libconfig-configure.log"; exit 1; }
    make -j"$JOBS" > "$LOGDIR/libconfig-make.log" 2>&1 \
      || { bad "make failed, see $LOGDIR/libconfig-make.log"; exit 1; }
    make install > "$LOGDIR/libconfig-install.log" 2>&1 \
      || { bad "install failed, see $LOGDIR/libconfig-install.log"; exit 1; }
    ok "built and installed (autotools)"
  else
    bad "no CMakeLists.txt and no executable ./configure in libconfig -- cannot build"
    echo "  libtool is missing on this host; try a release tarball instead:"
    echo "  https://github.com/hyperrealm/libconfig/releases"
    exit 1
  fi
fi

# ------------------------------------------------------------------- verify
head1 "verify"
fail=0
for h in rtl-sdr.h libconfig.h; do
  if [ -f "$PREFIX/include/$h" ]; then ok "header $h"
  else bad "header $h MISSING"; fail=1; fi
done
for lib in librtlsdr libconfig; do
  if ls "$PREFIX"/lib*/"$lib".so* >/dev/null 2>&1; then
    ok "library $lib -> $(ls "$PREFIX"/lib*/"$lib".so* | head -1)"
  else
    bad "library $lib MISSING"; fail=1
  fi
done

# Confirm the fork really is the fork: these two symbols are the whole point.
if lib=$(ls "$PREFIX"/lib*/librtlsdr.so* 2>/dev/null | head -1); then
  for sym in rtlsdr_set_dithering rtlsdr_set_sample_freq_correction_f; do
    if nm -D --defined-only "$lib" 2>/dev/null | grep -qw "$sym"; then
      ok "symbol $sym"
    else
      bad "symbol $sym absent -- this may not be the krakenrf fork"; fail=1
    fi
  done
fi

head1 "use it"
cat <<EOF
  make CFLAGS="-I$PREFIX/include" \\
       LDFLAGS="-L$PREFIX/lib -Wl,-rpath,$PREFIX/lib"
EOF

exit $fail
