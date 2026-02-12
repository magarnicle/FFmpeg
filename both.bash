set -o errexit
set -o xtrace
set -o pipefail
while true
do
    ./ffmpeg -v debug -i test2.mov -i test2.srt -filter_complex "[0:v][1:s]cea608embed[vid]" -map "[vid]" -map "0:a" -map "1:s" -c:v v210 -c:s teletext -a53cc 1 -f decklink "DeckLink SDI 4K" 2>&1 | tee both.txt
   sleep 60
done
