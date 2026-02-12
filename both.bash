./ffmpeg -i test2.mov -i test2.srt -filter_complex "[0:v][1:s]cea608embed[vid]" -map "[vid]" -map "0:a" -map "1:s" -c:v v210 -c:s teletext -f decklink "DeckLink SDI 4K"
#-a53cc 1 
