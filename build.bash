
PATH="$HOME/bin:$PATH" PKG_CONFIG_PATH="$HOME/ffmpeg_build/lib/pkgconfig" ./configure \
--prefix="$HOME/ffmpeg_build" \
--pkg-config-flags="--static" \
--extra-cflags="-I$HOME/ffmpeg_build/include -I$HOME/ffmpeg_sources/Blackmagic_DeckLink_SDK_15.0/Linux/include" \
--extra-ldflags="-L$HOME/ffmpeg_build/lib" \
--extra-libs="-lpthread -lm" \
--bindir="$HOME/bin" \
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
--enable-nonfree \
--enable-decklink
#--enable-swscale-alpha
# --enable-openssl \
# --enable-libfreetype # for graphics
# --enable-libharfbuzz # for graphics
# --enable-libfontconfig # for graphics
# --enable-libfribidi # for graphics
# --enable-openssl for https streams
PATH="$HOME/bin:$PATH" make -j `nproc`
#sudo cp ffmpeg ffprobe /usr/local/bin/
