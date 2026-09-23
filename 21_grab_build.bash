#!/bin/bash
# Build tools/ff21grab, the DeckLink SD PAL VBI dump.
#
# The DeckLink SDK lives in a different place on each machine, so look rather
# than hard-code: the copy in this tree first, then the usual build location,
# then whatever ffbuild/config.mak was configured with.
set -e

if [ -n "$SDK" ] && [ ! -r "$SDK/DeckLinkAPI.h" ]; then
    echo "SDK=$SDK does not contain DeckLinkAPI.h" >&2
    exit 1
fi

for dir in ${SDK:+"$SDK"} \
    decklink/Blackmagic_DeckLink_SDK_15.0/Linux/include \
    "$HOME"/ffmpeg_sources/Blackmagic_DeckLink_SDK_*/Linux/include \
    $(sed -n 's/.*-I\([^ ]*Blackmagic[^ ]*include\).*/\1/p' ffbuild/config.mak 2>/dev/null | tr -d "'" | sort -u)
do
    if [ -r "$dir/DeckLinkAPI.h" ]; then SDK="$dir"; break; fi
done

if [ -z "$SDK" ]; then
    echo "Could not find DeckLinkAPI.h. Pass the SDK include directory:" >&2
    echo "  SDK=/path/to/Blackmagic_DeckLink_SDK/Linux/include $0" >&2
    exit 1
fi

echo "Using DeckLink SDK: $SDK"
g++ -O2 -std=c++11 -o tools/ff21grab tools/ff21grab.cpp \
    "$SDK/DeckLinkAPIDispatch.cpp" \
    -I"$SDK" -ldl -lpthread
echo "Built tools/ff21grab"
