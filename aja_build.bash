#!/bin/bash
# Build ffmpeg with both the DeckLink and AJA outputs.
#
# The NTV2 SDK is C++ and spreads its headers over several directories, and
# ajatypes.h needs the platform define, so all of that goes in extra-cxxflags.
# build/ajantv2 holds the generated ntv2version.h, so libajantv2 must have been
# built at least once:
#     cmake -S libajantv2 -B libajantv2/build -DCMAKE_BUILD_TYPE=Release
#     cmake --build libajantv2/build -j$(nproc) --target ajantv2
set -e

AJA=${AJA:-$PWD/libajantv2}
[ -r "$AJA/ajantv2/includes/ntv2card.h" ] || { echo "No NTV2 SDK at $AJA" >&2; exit 1; }
[ -r "$AJA/build/ajantv2/ntv2version.h" ] || echo "WARNING: $AJA/build/ajantv2/ntv2version.h missing; build libajantv2 first" >&2

AJA_CXXFLAGS="-DAJALinux -DAJA_LINUX -DAJASTATIC -DAJA_USE_CPLUSPLUS11 -DNTV2_USE_CPLUSPLUS11"
AJA_CXXFLAGS="$AJA_CXXFLAGS -I$AJA/ajantv2/includes -I$AJA -I$AJA/ajabase"
AJA_CXXFLAGS="$AJA_CXXFLAGS -I$AJA/ajantv2/src/lin -I$AJA/build/ajantv2"
AJA_CXXFLAGS="$AJA_CXXFLAGS -I$AJA/ajabase/pnp/linux -I$AJA/ajabase/system/linux"

PATH="$HOME/bin:$PATH" PKG_CONFIG_PATH="$HOME/ffmpeg_build/lib/pkgconfig:/usr/bin/pkg-config" ./configure \
--prefix="$HOME/ffmpeg_build" \
--pkg-config-flags="--static" \
--extra-cflags="-I$HOME/ffmpeg_build/include -I$HOME/ffmpeg_sources/Blackmagic_DeckLink_SDK_15.0/Linux/include" \
--extra-cxxflags="-I$HOME/ffmpeg_build/include -I$HOME/ffmpeg_sources/Blackmagic_DeckLink_SDK_15.0/Linux/include $AJA_CXXFLAGS" \
--extra-ldflags="-L$HOME/ffmpeg_build/lib -L$AJA/build/ajantv2" \
--extra-libs="-lpthread -lstdc++exp -lm" \
--bindir="$HOME/bin" \
--enable-debug=3 \
--disable-stripping \
--enable-gpl \
--enable-libass \
--enable-libfdk-aac \
--enable-libfreetype \
--enable-libharfbuzz \
--enable-libfontconfig \
--enable-libfribidi \
--enable-libmp3lame \
--enable-libopus \
--enable-libvorbis \
--enable-libvpx \
--enable-libx264 \
--enable-libx265 \
--enable-libklvanc \
--enable-nonfree \
--enable-openssl \
--enable-ajantv2 \
--enable-decklink

PATH="$HOME/bin:$PATH" make -j `nproc`
