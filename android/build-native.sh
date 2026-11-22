#!/bin/sh
# Builds toxic and everything it needs for one Android ABI into app/src/main/jniLibs.
#
# Usage: build-native.sh ABI
#
# Needs ANDROID_NDK_HOME, cmake, make, perl, pkg-config, tic and infocmp. c-toxcore comes from
# TOXCORE_SRC if set, otherwise its master is cloned. Everything is built in BUILD_DIR, by default
# android/build.
set -eu

ABI=$1
API=26
case $ABI in
  arm64-v8a) TARGET=aarch64-linux-android OPENSSL_TARGET=android-arm64 VPX_TARGET=arm64-android-gcc ;;
  armeabi-v7a) TARGET=armv7a-linux-androideabi OPENSSL_TARGET=android-arm VPX_TARGET=armv7-android-gcc ;;
  x86_64) TARGET=x86_64-linux-android OPENSSL_TARGET=android-x86_64 VPX_TARGET=x86_64-android-gcc ;;
  x86) TARGET=i686-linux-android OPENSSL_TARGET=android-x86 VPX_TARGET=x86-android-gcc ;;
  *) echo "unknown ABI: $ABI" >&2 && exit 1 ;;
esac

HERE=$(cd "$(dirname "$0")" && pwd)
TOXIC=$(dirname "$HERE")
BUILD_DIR=${BUILD_DIR:-$HERE/build}
BUILD=$BUILD_DIR/$ABI
PREFIX=$BUILD/prefix
DOWNLOADS=$BUILD_DIR/downloads
JNILIBS=$HERE/app/src/main/jniLibs/$ABI
NDK=$ANDROID_NDK_HOME
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
JOBS=$(nproc)

export CC=$TOOLCHAIN/bin/$TARGET$API-clang
export CXX=$TOOLCHAIN/bin/$TARGET$API-clang++
export AR=$TOOLCHAIN/bin/llvm-ar
export RANLIB=$TOOLCHAIN/bin/llvm-ranlib
export STRIP=$TOOLCHAIN/bin/llvm-strip
export CFLAGS="-O2 -fPIC"
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig
export PKG_CONFIG_PATH=$PKG_CONFIG_LIBDIR
CMAKE_ANDROID="-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=$ABI
  -DANDROID_PLATFORM=android-$API -DCMAKE_INSTALL_PREFIX=$PREFIX -DCMAKE_PREFIX_PATH=$PREFIX
  -DCMAKE_FIND_ROOT_PATH=$PREFIX;$TOOLCHAIN/sysroot -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF -DANDROID_STL=c++_static"

mkdir -p "$BUILD" "$DOWNLOADS" "$JNILIBS"

# Whether $1 still needs building.
todo() {
  [ ! -f "$BUILD/$1.done" ]
}

# Downloads a tarball, checks its hash, unpacks it into $BUILD/$1 and changes to that directory.
unpack() {
  name=$1 url=$2 sha256=$3
  file=$DOWNLOADS/$name-$(basename "$url")
  [ -f "$file" ] || curl -fsSLo "$file" "$url"
  echo "$sha256  $file" | sha256sum -c
  rm -rf "${BUILD:?}/$name" && mkdir -p "$BUILD/$name"
  tar xf "$file" -C "$BUILD/$name" --strip-components=1
  cd "$BUILD/$name"
}

if todo libsodium; then
  unpack libsodium https://github.com/jedisct1/libsodium/releases/download/1.0.22-RELEASE/libsodium-1.0.22.tar.gz \
    adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349
  ./configure --host="$TARGET" --prefix="$PREFIX" --disable-shared --with-pic
  make -j"$JOBS" install
  touch "$BUILD/libsodium.done"
fi

# The terminal entry for our emulator is built into ncurses, so it needs no terminfo files.
if todo ncurses; then
  unpack ncurses https://invisible-island.net/archives/ncurses/ncurses-6.6.tar.gz \
    355b4cbbed880b0381a04c46617b7656e362585d52e9cf84a67e2009b749ff11
  ./configure --host="$TARGET" --prefix="$PREFIX" --with-build-cc=cc \
    --enable-widec --without-shared --without-cxx --without-ada --without-manpages --without-progs \
    --without-tests --disable-database \
    --with-fallbacks=toxic-android --with-tic-path="$(command -v tic)" \
    --with-infocmp-path="$(command -v infocmp)" --enable-pc-files \
    --with-pkg-config-libdir="$PREFIX/lib/pkgconfig"
  make -j"$JOBS" TERMINFO_SRC="$HERE/toxic.terminfo" install
  touch "$BUILD/ncurses.done"
fi

if todo libconfig; then
  unpack libconfig https://github.com/hyperrealm/libconfig/archive/refs/tags/v1.8.3.tar.gz \
    683f723d647befcdb36e1a4a61a83ae8c89dc7959ca872a7407b6c8152104162
  # shellcheck disable=SC2086
  cmake -B _build $CMAKE_ANDROID -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_CXX=OFF
  cmake --build _build -j"$JOBS" --target install
  touch "$BUILD/libconfig.done"
fi

if todo openssl; then
  unpack openssl https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz \
    603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
  PATH=$TOOLCHAIN/bin:$PATH ANDROID_NDK_ROOT=$NDK CC=clang ./Configure "$OPENSSL_TARGET" -D__ANDROID_API__=$API \
    --prefix="$PREFIX" --libdir=lib no-shared no-tests no-docs
  PATH=$TOOLCHAIN/bin:$PATH ANDROID_NDK_ROOT=$NDK CC=clang make -j"$JOBS"
  PATH=$TOOLCHAIN/bin:$PATH make install_sw
  touch "$BUILD/openssl.done"
fi

# No CA bundle: session.c gives each curl handle the device's certificates.
if todo curl; then
  unpack curl https://curl.se/download/curl-8.21.0.tar.xz \
    aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6
  ./configure --host="$TARGET" --prefix="$PREFIX" --disable-shared --with-pic --with-openssl="$PREFIX" \
    --without-ca-bundle --without-ca-path --with-ca-fallback --without-libpsl --without-zlib \
    --without-brotli --without-zstd --without-nghttp2 --without-libidn2 --disable-docs --disable-manual
  make -j"$JOBS" install
  touch "$BUILD/curl.done"
fi

if todo opus; then
  unpack opus https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz \
    6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
  ./configure --host="$TARGET" --prefix="$PREFIX" --disable-shared --with-pic --disable-doc \
    --disable-extra-programs
  make -j"$JOBS" install
  touch "$BUILD/opus.done"
fi

if todo libvpx; then
  unpack libvpx https://storage.googleapis.com/downloads.webmproject.org/releases/webm/libvpx-1.17.0.tar.gz \
    7f98aae5c52c46e85b7b26feabcab9b1554605e771bd1df1d6e6a00ee22fa8ed
  ./configure --prefix="$PREFIX" --target="$VPX_TARGET" --libc="$TOOLCHAIN/sysroot" --disable-vp9 \
    --disable-examples --disable-tools --disable-docs --disable-unit-tests --enable-pic
  # libvpx's Android targets assume GNU as and strip.
  sed -i -e "s|^AS=as|AS=$CC -c|" -e "s|^STRIP=strip|STRIP=$STRIP|" ./*.mk
  make -j"$JOBS" install
  touch "$BUILD/libvpx.done"
fi

# Play calls on the voice stream and record with the voice preset, which has echo cancellation.
if todo openal-soft; then
  unpack openal-soft https://github.com/kcat/openal-soft/archive/refs/tags/1.25.2.tar.gz \
    fb27e5839aa11f0e5b9d33756965291fad5d6909ab928ea1f796f4a1a6877894
  sed -i -e 's/SL_ANDROID_STREAM_MEDIA/SL_ANDROID_STREAM_VOICE/' \
    -e 's/SL_ANDROID_RECORDING_PRESET_GENERIC/SL_ANDROID_RECORDING_PRESET_VOICE_COMMUNICATION/' \
    alc/backends/opensl.cpp
  # shellcheck disable=SC2086
  cmake -B _build $CMAKE_ANDROID -DLIBTYPE=STATIC -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF \
    -DALSOFT_INSTALL_CONFIG=OFF -DALSOFT_INSTALL_HRTF_DATA=OFF -DALSOFT_INSTALL_AMBDEC_PRESETS=OFF \
    -DALSOFT_BACKEND_OBOE=OFF -DALSOFT_REQUIRE_OPENSL=ON \
    -DOPENSL_INCLUDE_DIR="$TOOLCHAIN/sysroot/usr/include" \
    -DOPENSL_ANDROID_INCLUDE_DIR="$TOOLCHAIN/sysroot/usr/include"
  cmake --build _build -j"$JOBS" --target install
  touch "$BUILD/openal-soft.done"
fi

if todo c-toxcore; then
  rm -rf "$BUILD/c-toxcore"
  if [ -n "${TOXCORE_SRC-}" ]; then
    cp -r "$TOXCORE_SRC" "$BUILD/c-toxcore"
  else
    git clone --depth=1 --recursive https://github.com/TokTok/c-toxcore "$BUILD/c-toxcore"
  fi
  cd "$BUILD/c-toxcore"
  # shellcheck disable=SC2086
  cmake -B _build $CMAKE_ANDROID -DENABLE_SHARED=OFF -DBOOTSTRAP_DAEMON=OFF -DDHT_BOOTSTRAP=OFF \
    -DMUST_BUILD_TOXAV=ON
  cmake --build _build -j"$JOBS" --target install
  touch "$BUILD/c-toxcore.done"
fi

# Build toxic and link it with our JNI code into libtoxic.so, the one library the app loads.
rm -rf "$BUILD/toxic" && mkdir -p "$BUILD/toxic"
cd "$TOXIC"
git ls-files -z -co --exclude-standard Makefile cfg misc sounds src | xargs -0 cp --parents -t "$BUILD/toxic"
for c in "$HERE"/app/src/main/c/*.c; do
  "$CC" $CFLAGS -std=c11 -Wall -Wextra -I"$PREFIX/include" -I"$TOXIC/src" -c "$c" -o "$BUILD/$(basename "$c" .c).o"
done
cd "$BUILD/toxic"
# UNAME_S=Android: the Makefile looks at the build machine's OS, and its Linux flags link librt,
# which Android doesn't have. The X11 check reports that X11 is missing: video on Android doesn't
# use it.
make -j"$JOBS" UNAME_S=Android ENABLE_RELEASE=1 DISABLE_SOUND_NOTIFY=1 \
  DISABLE_DESKTOP_NOTIFY=1 DISABLE_QRCODE=1 DISABLE_QRPNG=1 USER_CFLAGS=-fPIC \
  USER_LDFLAGS="-shared -Wl,-z,max-page-size=16384 -Wl,--wrap=curl_easy_init -Wl,--wrap=exit -Wl,--wrap=setlocale \
    -Wl,--wrap=alcCaptureOpenDevice -Wl,--wrap=alcCaptureCloseDevice $BUILD/*.o \
    $(pkg-config --static --libs toxcore ncursesw libconfig libcurl openal vpx) \
    -lc++_static -lc++abi -llog -landroid"
"$STRIP" -o "$JNILIBS/libtoxic.so" build/toxic
