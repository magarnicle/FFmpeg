# Teletext encoder tests

TELETEXT_SRT = $(TARGET_PATH)/tests/data/sub-cea608embed.srt

FATE_TELETEXT-$(call ENCMUX, TELETEXT, DATA, SRT_DEMUXER FILE_PROTOCOL PIPE_PROTOCOL) += fate-teletext-encode
fate-teletext-encode: CMD = md5 -i $(TELETEXT_SRT) -c:s teletext -teletext_page 100 -f data

FATE_FFMPEG += $(FATE_TELETEXT-yes)
