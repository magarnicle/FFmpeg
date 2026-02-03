set -o xtrace
if false
then
    if [[ $# -gt 0 ]]
    then
        gdb -batch -ex "run" -ex "bt" --args ./ffmpeg -loglevel debug -y -i AU109958-M.mov -i AU109958-M.srt -i AU109958-M.mov -i AU109958-M.srt -filter_complex "[0:v][0:a][1:s][2:v][2:a][3:s]concat=n=2:v=1:a=1:s=1[vid][aud][sub]" -map "[vid]" -map "[aud]" -map "[sub]" -c:v v210 -c:s teletext -f decklink "DeckLink SDI 4K" 2>&1
    else
        ./ffmpeg -loglevel debug -y -i AU109958-M.mov -i AU109958-M.srt -i AU109958-M.mov -i AU109958-M.srt -filter_complex "[0:v][0:a][1:s][2:v][2:a][3:s]concat=n=2:v=1:a=1:s=1[vid][aud][sub]" -map "[vid]" -map "[aud]" -map "[sub]" -c:v v210 -c:s teletext -f decklink "DeckLink SDI 4K" 2>&1
    fi
else
    ./ffmpeg -loglevel debug -y -i AU109958-M.mov -i AU109958-M.srt -i AU109958-M.mov -i AU109958-M.srt -filter_complex "[0:v][0:a][1:s][2:v][2:a][3:s]concat=n=2:v=1:a=1:s=1[vid][aud][sub]" -map "[vid]" -map "[aud]" -map "[sub]" test.mov 2>&1
fi
