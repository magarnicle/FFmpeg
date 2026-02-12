# Drawtext canvas viewport tests

FATE_FILTER_DRAWTEXT_CANVAS-$(call FILTERFRAMECRC, TESTSRC DRAWTEXT) += fate-filter-drawtext-canvas
fate-filter-drawtext-canvas: CMD = framecrc -lavfi testsrc=r=5:d=1,drawtext=text=HelloWorld:canvas_w=100:canvas_h=30:canvas_x=10:canvas_y=10 -frames:v 5

FATE_FILTER_DRAWTEXT_CANVAS-$(call FILTERFRAMECRC, TESTSRC DRAWTEXT) += fate-filter-drawtext-canvas-tile
fate-filter-drawtext-canvas-tile: CMD = framecrc -lavfi testsrc=r=5:d=1,drawtext=text=TILE:canvas_w=50:canvas_h=20:canvas_tile=1 -frames:v 5

FATE_FILTER-yes += $(FATE_FILTER_DRAWTEXT_CANVAS-yes)
