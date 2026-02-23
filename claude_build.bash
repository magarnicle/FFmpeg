if false
then
PATH="$HOME/bin:$PATH" PKG_CONFIG_PATH="/usr/src/ffmpeg/build/lib/pkgconfig:/usr/bin/pkg-config" ./configure \
--prefix="/usr/src/ffmpeg/build" \
--pkg-config-flags="--static" \
--extra-cflags="-I/usr/src/ffmpeg/build/include -I/usr/src/ffmpeg/decklink/Blackmagic_DeckLink_SDK_15.0/Linux/include" \
--extra-cxxflags="-I/usr/src/ffmpeg/build/include -I/usr/src/ffmpeg/decklink/Blackmagic_DeckLink_SDK_15.0/Linux/include" \
--extra-ldflags="-L/usr/src/ffmpeg/build/lib" \
--extra-libs="-lpthread -lstdc++exp -lm" \
--bindir="$HOME/bin"
#--enable-swscale-alpha
# --enable-openssl \
# --enable-libfreetype # for graphics
# --enable-libharfbuzz # for graphics
# --enable-libfontconfig # for graphics
# --enable-libfribidi # for graphics
# --enable-openssl for https streams
PATH="$HOME/bin:$PATH" make -j `nproc`
#sudo cp ffmpeg ffprobe /usr/local/bin/
#--extra-libs="-lpthread -lstdc++_libbacktrace -lm" \
else
PATH="$HOME/bin:$PATH" PKG_CONFIG_PATH="/usr/src/ffmpeg/build/lib/pkgconfig:/usr/bin/pkg-config" ./configure \
--prefix="/usr/src/ffmpeg/build" \
--pkg-config-flags="--static" \
--extra-cflags="-I/usr/src/ffmpeg/build/include -I/usr/src/ffmpeg/decklink/Blackmagic_DeckLink_SDK_15.0/Linux/include" \
--extra-cxxflags="-I/usr/src/ffmpeg/build/include -I/usr/src/ffmpeg/decklink/Blackmagic_DeckLink_SDK_15.0/Linux/include" \
--extra-ldflags="-L/usr/src/ffmpeg/build/lib" \
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
--enable-nonfree
#--enable-swscale-alpha
# --enable-openssl \
# --enable-libfreetype # for graphics
# --enable-libharfbuzz # for graphics
# --enable-libfontconfig # for graphics
# --enable-libfribidi # for graphics
# --enable-openssl for https streams
PATH="$HOME/bin:$PATH" make -j `nproc`
#sudo cp ffmpeg ffprobe /usr/local/bin/
#--extra-libs="-lpthread -lstdc++_libbacktrace -lm" \
fi
