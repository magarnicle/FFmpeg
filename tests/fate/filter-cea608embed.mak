# CEA-608 closed caption embedding filter tests

CEA608EMBED_SRT = $(TARGET_PATH)/tests/data/sub-cea608embed.srt

FATE_FILTER_CEA608EMBED += fate-filter-cea608embed-popon
fate-filter-cea608embed-popon: CMD = framecrc -lavfi "testsrc=r=30:d=2,cea608embed=filename=$(CEA608EMBED_SRT):roll_up=0" -frames:v 60

FATE_FILTER_CEA608EMBED += fate-filter-cea608embed-rollup
fate-filter-cea608embed-rollup: CMD = framecrc -lavfi "testsrc=r=30:d=2,cea608embed=filename=$(CEA608EMBED_SRT):roll_up=2" -frames:v 60

FATE_FILTER-$(call FILTERFRAMECRC, TESTSRC CEA608EMBED, FILE_PROTOCOL SRT_DEMUXER) += $(FATE_FILTER_CEA608EMBED)
