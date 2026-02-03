./ffmpeg -y -i AU109958-M.srt -i AU109958-M.srt -filter_complex [0:s][1:s]concat=n=2:v=0:a=0:s=1[sub] -map [sub] test.srt 2>&1
