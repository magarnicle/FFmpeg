/*
 * Copyright (c) 2023 Francesco Carusi
 * Copyright (c) 2011 Stefano Sabatini
 * Copyright (c) 2010 S.N. Hemanth Meenakshisundaram
 * Copyright (c) 2003 Gustavo Sverzut Barbieri <gsbarbieri@yahoo.com.br>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * drawtext filter, based on the original vhook/drawtext.c
 * filter by Gustavo Sverzut Barbieri
 */

#include "config.h"

#if HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <stdio.h>
#include <inttypes.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <fenv.h>

#if CONFIG_LIBFONTCONFIG
#include <fontconfig/fontconfig.h>
#endif

#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/random_seed.h"
#include "libavutil/parseutils.h"
#include "libavutil/time.h"
#include "libavutil/timecode.h"
#include "libavutil/time_internal.h"
#include "libavutil/tree.h"
#include "libavutil/lfg.h"
#include "libavutil/detection_bbox.h"
#include "avfilter.h"
#include "drawutils.h"
#include "filters.h"
#include "formats.h"
#include "textutils.h"
#include "video.h"

#if CONFIG_LIBFRIBIDI
#include <fribidi.h>
#endif

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_STROKER_H

#include <hb.h>
#include <hb-ft.h>
#include <hb-subset.h>

// Ceiling operation for positive integers division
#define POS_CEIL(x, y) ((x)/(y) + ((x)%(y) != 0))

static const char *const var_names[] = {
    "dar",
    "hsub", "vsub",
    "line_h", "lh",           ///< line height
    "main_h", "h", "H",       ///< height of the input video
    "main_w", "w", "W",       ///< width  of the input video
    "max_glyph_a", "ascent",  ///< max glyph ascender
    "max_glyph_d", "descent", ///< min glyph descender
    "max_glyph_h",            ///< max glyph height
    "max_glyph_w",            ///< max glyph width
    "font_a",                 ///< font-defined ascent
    "font_d",                 ///< font-defined descent
    "top_a",                  ///< max glyph ascender of the top line
    "bottom_d",               ///< max glyph descender of the bottom line
    "n",                      ///< number of frame
    "sar",
    "t",                      ///< timestamp expressed in seconds
    "text_h", "th",           ///< height of the rendered text
    "text_w", "tw",           ///< width  of the rendered text
    "x",
    "y",
    "pict_type",
    "duration",
    NULL
};

static const char *const fun2_names[] = {
    "rand"
};

static double drand(void *opaque, double min, double max)
{
    return min + (max-min) / UINT_MAX * av_lfg_get(opaque);
}

typedef double (*eval_func2)(void *, double a, double b);

static const eval_func2 fun2[] = {
    drand,
    NULL
};

enum var_name {
    VAR_DAR,
    VAR_HSUB, VAR_VSUB,
    VAR_LINE_H, VAR_LH,
    VAR_MAIN_H, VAR_h, VAR_H,
    VAR_MAIN_W, VAR_w, VAR_W,
    VAR_MAX_GLYPH_A, VAR_ASCENT,
    VAR_MAX_GLYPH_D, VAR_DESCENT,
    VAR_MAX_GLYPH_H,
    VAR_MAX_GLYPH_W,
    VAR_FONT_A,
    VAR_FONT_D,
    VAR_TOP_A,
    VAR_BOTTOM_D,
    VAR_N,
    VAR_SAR,
    VAR_T,
    VAR_TEXT_H, VAR_TH,
    VAR_TEXT_W, VAR_TW,
    VAR_X,
    VAR_Y,
    VAR_PICT_TYPE,
    VAR_DURATION,
    VAR_VARS_NB
};

enum expansion_mode {
    EXP_NONE,
    EXP_NORMAL,
    EXP_STRFTIME,
};

enum y_alignment {
    YA_TEXT,
    YA_BASELINE,
    YA_FONT,
};

enum text_alignment {
    TA_LEFT   = (1 << 0),
    TA_RIGHT  = (1 << 1),
    TA_TOP    = (1 << 2),
    TA_BOTTOM = (1 << 3),
};

typedef struct HarfbuzzData {
    hb_buffer_t* buf;
    unsigned int glyph_count;
    hb_glyph_info_t* glyph_info;
    hb_glyph_position_t* glyph_pos;
    int owns_arrays;                ///< 1 if glyph_info/glyph_pos are owned (from cache), 0 if from hb_buffer
} HarfbuzzData;

/** Information about a single glyph in a text line */
typedef struct GlyphInfo {
    uint32_t code;                  ///< the glyph code point
    int x;                          ///< the x position of the glyph
    int y;                          ///< the y position of the glyph
    int shift_x64;                  ///< the horizontal shift of the glyph in 26.6 units
    int shift_y64;                  ///< the vertical shift of the glyph in 26.6 units
} GlyphInfo;

/** Information about a single line of text */
typedef struct TextLine {
    int offset_left64;              ///< offset between the origin and
                                    ///  the leftmost pixel of the first glyph
    int offset_right64;             ///< maximum offset between the origin and
                                    ///  the rightmost pixel of the last glyph
    int width64;                    ///< width of the line
    int width_px;                   ///< pixel width of the line: POS_CEIL(width64, 64)
    HarfbuzzData hb_data;           ///< libharfbuzz data of this text line
    GlyphInfo* glyphs;              ///< array of glyphs in this text line
    int glyphs_alloc;               ///< allocated capacity of glyphs[] in GlyphInfo units
    int cluster_offset;             ///< the offset at which this line begins
    int is_shaped;                  ///< 1 if shaped, 0 if skipped for viewport optimization
    const char *text_start;         ///< pointer to start of line in text buffer
    int text_len;                   ///< byte length of this line
    int first_tab_idx;              ///< index into s->tab_clusters of first tab in this line
    int line_tab_count;             ///< number of tab characters in this line

    // Per-line glyph metrics cache; valid when metrics_fontsize == s->fontsize.
    // Cleared to 0 (invalid) whenever the lines[] array is rebuilt on text change.
    unsigned int metrics_fontsize;  ///< fontsize when the fields below were last computed
    int line_min_y64;               ///< min bbox.yMin across all shaped glyphs
    int line_max_y64;               ///< max bbox.yMax across all shaped glyphs
    int line_min_x64;               ///< min bbox.xMin across all shaped glyphs
    int line_max_x64;               ///< max bbox.xMax across all shaped glyphs
} TextLine;

/** A glyph as loaded and rendered using libfreetype */
typedef struct Glyph {
    FT_Glyph glyph;
    FT_Glyph border_glyph;
    uint32_t code;
    unsigned int fontsize;
    /** Glyph bitmaps with 1/4 pixel precision in both directions */
    FT_BitmapGlyph bglyph[16];
    /** Outlined glyph bitmaps with 1/4 pixel precision in both directions */
    FT_BitmapGlyph border_bglyph[16];
    /** Bitmask: bit i set if bglyph[i] was loaded from disk cache (needs av_free) */
    uint16_t bglyph_from_cache;
    /** Bitmask: bit i set if border_bglyph[i] was loaded from disk cache */
    uint16_t border_bglyph_from_cache;
    FT_BBox bbox;
    struct Glyph *next;             ///< next glyph in hash bucket
} Glyph;

#define GLYPH_HASH_SIZE 256

/** Hash table for O(1) glyph lookup */
typedef struct GlyphHash {
    Glyph *buckets[GLYPH_HASH_SIZE];
} GlyphHash;

/** Global text metrics */
typedef struct TextMetrics {
    int offset_top64;               ///< ascender amount of the first line (in 26.6 units)
    int offset_bottom64;            ///< descender amount of the last line (in 26.6 units)
    int offset_left64;              ///< maximum offset between the origin and
                                    ///  the leftmost pixel of the first glyph
                                    ///  of each line (in 26.6 units)
    int offset_right64;             ///< maximum offset between the origin and
                                    ///  the rightmost pixel of the last glyph
                                    ///  of each line (in 26.6 units)
    int line_height64;              ///< the font-defined line height
    int width;                      ///< width of the longest line - ceil(width64/64)
    int height;                     ///< total height of the text - ceil(height64/64)

    int min_y64;                    ///< minimum value of bbox.yMin among glyphs (in 26.6 units)
    int max_y64;                    ///< maximum value of bbox.yMax among glyphs (in 26.6 units)
    int min_x64;                    ///< minimum value of bbox.xMin among glyphs (in 26.6 units)
    int max_x64;                    ///< maximum value of bbox.xMax among glyphs (in 26.6 units)

    // Position of the background box (without borders)
    int rect_x;                     ///< x position of the box
    int rect_y;                     ///< y position of the box
} TextMetrics;

#define WORD_CACHE_HASH_SIZE 512

/** In-memory hash table for word bitmap cache (forward declaration) */
typedef struct WordCacheHash {
    struct WordCacheEntry *buckets[WORD_CACHE_HASH_SIZE];
    int entry_count;        ///< total number of entries
    size_t total_bytes;     ///< total bitmap memory used
} WordCacheHash;

typedef struct DrawTextContext {
    const AVClass *class;
    int exp_mode;                   ///< expansion mode to use for the text
    FFExpandTextContext expand_text; ///< expand text in case exp_mode == NORMAL
    int reinit;                     ///< tells if the filter is being reinited
#if CONFIG_LIBFONTCONFIG
    uint8_t *font;                  ///< font to be used
#endif
    uint8_t *fontfile;              ///< font to be used
    uint8_t *text;                  ///< text to be drawn
    AVBPrint expanded_text;         ///< used to contain the expanded text
    uint8_t *fontcolor_expr;        ///< fontcolor expression to evaluate
    AVBPrint expanded_fontcolor;    ///< used to contain the expanded fontcolor spec
    int ft_load_flags;              ///< flags used for loading fonts, see FT_LOAD_*
    char *textfile;                 ///< file with text to be drawn
    double x;                       ///< x position to start drawing text
    double y;                       ///< y position to start drawing text
    int max_glyph_w;                ///< max glyph width
    int max_glyph_h;                ///< max glyph height
    int shadowx, shadowy;
    int borderw;                    ///< border width
    char *fontsize_expr;            ///< expression for fontsize
    AVExpr *fontsize_pexpr;         ///< parsed expressions for fontsize
    unsigned int fontsize;          ///< font size to use
    unsigned int default_fontsize;  ///< default font size to use

    int line_spacing;               ///< lines spacing in pixels
    short int draw_box;             ///< draw box around text - true or false
    char *boxborderw;               ///< box border width (padding)
                                    ///  allowed formats: "all", "vert|oriz", "top|right|bottom|left"
    int bb_top;                     ///< the size of the top box border
    int bb_right;                   ///< the size of the right box border
    int bb_bottom;                  ///< the size of the bottom box border
    int bb_left;                    ///< the size of the left box border
    int box_width;                  ///< the width of box
    int box_height;                 ///< the height of box
    int tabsize;                    ///< tab size
    int fix_bounds;                 ///< do we let it go out of frame bounds - t/f

    FFDrawContext dc;
    FFDrawColor fontcolor;          ///< foreground color
    FFDrawColor shadowcolor;        ///< shadow color
    FFDrawColor bordercolor;        ///< border color
    FFDrawColor boxcolor;           ///< background color

    FT_Library library;             ///< freetype font library handle
    FT_Face face;                   ///< freetype font face handle
    FT_Stroker stroker;             ///< freetype stroker handle
    GlyphHash glyph_hash;           ///< hash table for O(1) glyph lookup
    char *x_expr;                   ///< expression for x position
    char *y_expr;                   ///< expression for y position
    AVExpr *x_pexpr, *y_pexpr;      ///< parsed expressions for x and y
    int64_t basetime;               ///< base pts time in the real world for display
    double var_values[VAR_VARS_NB];
    char   *a_expr;
    AVExpr *a_pexpr;
    int alpha;
    AVLFG  prng;                    ///< random
    char       *tc_opt_string;      ///< specified timecode option string
    AVRational  tc_rate;            ///< frame rate for timecode
    AVTimecode  tc;                 ///< timecode context
    int tc24hmax;                   ///< 1 if timecode is wrapped to 24 hours, 0 otherwise
    int reload;                     ///< reload text file at specified frame interval
    int start_number;               ///< starting frame number for n/frame_num var
    char *text_source_string;       ///< the string to specify text data source
    enum AVFrameSideDataType text_source;
#if CONFIG_LIBFRIBIDI
    int text_shaping;               ///< 1 to shape the text before drawing it
#endif
    AVDictionary *metadata;

    int boxw;                       ///< the value of the boxw parameter
    int boxh;                       ///< the value of the boxh parameter
    int text_align;                 ///< the horizontal and vertical text alignment
    int y_align;                    ///< the value of the y_align parameter

    char *canvas_w_expr;            ///< expression for canvas width
    char *canvas_h_expr;            ///< expression for canvas height
    char *canvas_x_expr;            ///< expression for canvas x viewport offset
    char *canvas_y_expr;            ///< expression for canvas y viewport offset
    AVExpr *canvas_w_pexpr;         ///< parsed expression for canvas width
    AVExpr *canvas_h_pexpr;         ///< parsed expression for canvas height
    AVExpr *canvas_x_pexpr;         ///< parsed expression for canvas x
    AVExpr *canvas_y_pexpr;         ///< parsed expression for canvas y
    int canvas_w;                   ///< evaluated canvas width (0 = disabled)
    int canvas_h;                   ///< evaluated canvas height (0 = disabled)
    int canvas_x;                   ///< evaluated canvas x viewport offset
    int canvas_y;                   ///< evaluated canvas y viewport offset
    int canvas_tile;                ///< loop/tile text when canvas extends beyond text
    int canvas_tile_reload;         ///< reload text file for each tile
    time_t canvas_tile_reload_mtime; ///< cached mtime for tile reload

    // Version counter for line-boundary cache invalidation
    int text_version;               ///< incremented whenever s->text is loaded or reloaded
    int lines_text_version;         ///< text_version when lines array was last built
    unsigned int lines_expanded_len;///< expanded_text.len when lines array was last built

    TextLine *lines;                ///< computed information about text lines
    int line_count;                 ///< the number of text lines
    uint32_t *tab_clusters;         ///< the position of tab characters in the text
    int tab_count;                  ///< the number of tab characters
    int blank_advance64;            ///< the size of the space character
    int tab_warning_printed;        ///< ensure the tab warning to be printed only once
    hb_font_t *hb_font;             ///< shared HarfBuzz font (owned by context, not per-line)
    hb_blob_t *hb_blob;             ///< memory-mapped font blob (for faster loading)
    hb_face_t *hb_face;             ///< HarfBuzz face created from blob

    // Tracks which lines were visible last frame so we can clear only the
    // lines that leave the viewport (not all 30k+ lines) each frame.
    int prev_first_visible_line;    ///< first_visible_line from previous frame (-1 = none)
    int prev_last_visible_line;     ///< last_visible_line from previous frame  (-1 = none)

    // Disk-based glyph cache
    char *glyph_cache_dir;          ///< directory for persistent glyph cache (NULL = disabled)
    char *font_path;                ///< resolved font file path (for cache keying)
    uint64_t font_hash;             ///< hash of fontfile content for cache keying
    int glyph_cache_initialized;    ///< 1 if font_hash computed and cache ready

    // In-memory word bitmap cache
    WordCacheHash word_cache;       ///< in-memory cache for rendered word bitmaps
} DrawTextContext;

#define OFFSET(x) offsetof(DrawTextContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
#define TFLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption drawtext_options[]= {
    {"fontfile",       "set font file",         OFFSET(fontfile),           AV_OPT_TYPE_STRING, {.str=NULL},  0, 0, FLAGS},
    {"text",           "set text",              OFFSET(text),               AV_OPT_TYPE_STRING, {.str=NULL},  0, 0, TFLAGS},
    {"textfile",       "set text file",         OFFSET(textfile),           AV_OPT_TYPE_STRING, {.str=NULL},  0, 0, FLAGS},
    {"fontcolor",      "set foreground color",  OFFSET(fontcolor.rgba),     AV_OPT_TYPE_COLOR,  {.str="black"}, 0, 0, TFLAGS},
    {"fontcolor_expr", "set foreground color expression", OFFSET(fontcolor_expr), AV_OPT_TYPE_STRING, {.str=""}, 0, 0, FLAGS},
    {"boxcolor",       "set box color",         OFFSET(boxcolor.rgba),      AV_OPT_TYPE_COLOR,  {.str="white"}, 0, 0, TFLAGS},
    {"bordercolor",    "set border color",      OFFSET(bordercolor.rgba),   AV_OPT_TYPE_COLOR,  {.str="black"}, 0, 0, TFLAGS},
    {"shadowcolor",    "set shadow color",      OFFSET(shadowcolor.rgba),   AV_OPT_TYPE_COLOR,  {.str="black"}, 0, 0, TFLAGS},
    {"box",            "set box",               OFFSET(draw_box),           AV_OPT_TYPE_BOOL,   {.i64=0},     0, 1, TFLAGS},
    {"boxborderw",     "set box borders width", OFFSET(boxborderw),         AV_OPT_TYPE_STRING, {.str="0"},   0, 0, TFLAGS},
    {"line_spacing",   "set line spacing in pixels", OFFSET(line_spacing),  AV_OPT_TYPE_INT,    {.i64=0},     INT_MIN, INT_MAX, TFLAGS},
    {"fontsize",       "set font size",         OFFSET(fontsize_expr),      AV_OPT_TYPE_STRING, {.str=NULL},  0, 0, TFLAGS},
    {"text_align",     "set text alignment",    OFFSET(text_align),         AV_OPT_TYPE_FLAGS,  {.i64=0}, 0, (TA_LEFT|TA_RIGHT|TA_TOP|TA_BOTTOM), TFLAGS, .unit = "text_align"},
        { "left",    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = TA_LEFT   }, .flags = TFLAGS, .unit = "text_align" },
        { "L",       NULL, 0, AV_OPT_TYPE_CONST, { .i64 = TA_LEFT   }, .flags = TFLAGS, .unit = "text_align" },
        { "right",   NULL, 0, AV_OPT_TYPE_CONST, { .i64 = TA_RIGHT  }, .flags = TFLAGS, .unit = "text_align" },
        { "R",       NULL, 0, AV_OPT_TYPE_CONST, { .i64 = TA_RIGHT  }, .flags = TFLAGS, .unit = "text_align" },
        { "center",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (TA_LEFT|TA_RIGHT) }, .flags = TFLAGS, .unit = "text_align" },
        { "C",       NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (TA_LEFT|TA_RIGHT) }, .flags = TFLAGS, .unit = "text_align" },
        { "top",     NULL, 0, AV_OPT_TYPE_CONST, { .i64 = TA_TOP    }, .flags = TFLAGS, .unit = "text_align" },
        { "T",       NULL, 0, AV_OPT_TYPE_CONST, { .i64 = TA_TOP    }, .flags = TFLAGS, .unit = "text_align" },
        { "bottom",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = TA_BOTTOM }, .flags = TFLAGS, .unit = "text_align" },
        { "B",       NULL, 0, AV_OPT_TYPE_CONST, { .i64 = TA_BOTTOM }, .flags = TFLAGS, .unit = "text_align" },
        { "middle",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (TA_TOP|TA_BOTTOM) }, .flags = TFLAGS, .unit = "text_align" },
        { "M",       NULL, 0, AV_OPT_TYPE_CONST, { .i64 = (TA_TOP|TA_BOTTOM) }, .flags = TFLAGS, .unit = "text_align" },
    {"x",              "set x expression",      OFFSET(x_expr),             AV_OPT_TYPE_STRING, {.str="0"},   0, 0, TFLAGS},
    {"y",              "set y expression",      OFFSET(y_expr),             AV_OPT_TYPE_STRING, {.str="0"},   0, 0, TFLAGS},
    {"boxw",           "set box width",         OFFSET(boxw),               AV_OPT_TYPE_INT,    {.i64=0},     0, INT_MAX, TFLAGS},
    {"boxh",           "set box height",        OFFSET(boxh),               AV_OPT_TYPE_INT,    {.i64=0},     0, INT_MAX, TFLAGS},
    {"canvas_w",       "set canvas width expression",  OFFSET(canvas_w_expr), AV_OPT_TYPE_STRING, {.str="w"}, 0, 0, TFLAGS},
    {"canvas_h",       "set canvas height expression", OFFSET(canvas_h_expr), AV_OPT_TYPE_STRING, {.str="h"}, 0, 0, TFLAGS},
    {"canvas_x",       "set canvas x offset expression", OFFSET(canvas_x_expr), AV_OPT_TYPE_STRING, {.str="0"}, 0, 0, TFLAGS},
    {"canvas_y",       "set canvas y offset expression", OFFSET(canvas_y_expr), AV_OPT_TYPE_STRING, {.str="0"}, 0, 0, TFLAGS},
    {"canvas_tile",    "tile text when canvas exceeds text bounds", OFFSET(canvas_tile), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, TFLAGS},
    {"canvas_tile_reload", "reload text file for each tile", OFFSET(canvas_tile_reload), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, TFLAGS},
    {"shadowx",        "set shadow x offset",   OFFSET(shadowx),            AV_OPT_TYPE_INT,    {.i64=0},     INT_MIN, INT_MAX, TFLAGS},
    {"shadowy",        "set shadow y offset",   OFFSET(shadowy),            AV_OPT_TYPE_INT,    {.i64=0},     INT_MIN, INT_MAX, TFLAGS},
    {"borderw",        "set border width",      OFFSET(borderw),            AV_OPT_TYPE_INT,    {.i64=0},     INT_MIN, INT_MAX, TFLAGS},
    {"tabsize",        "set tab size",          OFFSET(tabsize),            AV_OPT_TYPE_INT,    {.i64=4},     0, INT_MAX, TFLAGS},
    {"basetime",       "set base time",         OFFSET(basetime),           AV_OPT_TYPE_INT64,  {.i64=AV_NOPTS_VALUE}, INT64_MIN, INT64_MAX, FLAGS},
#if CONFIG_LIBFONTCONFIG
    { "font",        "Font name",            OFFSET(font),               AV_OPT_TYPE_STRING, { .str = "Sans" },           .flags = FLAGS },
#endif

    {"expansion", "set the expansion mode", OFFSET(exp_mode), AV_OPT_TYPE_INT, {.i64=EXP_NORMAL}, 0, 2, FLAGS, .unit = "expansion"},
        {"none",     "set no expansion",                    OFFSET(exp_mode), AV_OPT_TYPE_CONST, {.i64=EXP_NONE},     0, 0, FLAGS, .unit = "expansion"},
        {"normal",   "set normal expansion",                OFFSET(exp_mode), AV_OPT_TYPE_CONST, {.i64=EXP_NORMAL},   0, 0, FLAGS, .unit = "expansion"},
        {"strftime", "set strftime expansion (deprecated)", OFFSET(exp_mode), AV_OPT_TYPE_CONST, {.i64=EXP_STRFTIME}, 0, 0, FLAGS, .unit = "expansion"},
    {"y_align",   "set the y alignment",    OFFSET(y_align), AV_OPT_TYPE_INT,  {.i64=YA_TEXT}, 0, 2, TFLAGS, .unit = "y_align"},
        {"text",     "y is referred to the top of the first text line", OFFSET(y_align), AV_OPT_TYPE_CONST, {.i64=YA_TEXT},     0, 0, FLAGS, .unit = "y_align"},
        {"baseline", "y is referred to the baseline of the first line", OFFSET(y_align), AV_OPT_TYPE_CONST, {.i64=YA_BASELINE}, 0, 0, FLAGS, .unit = "y_align"},
        {"font",     "y is referred to the font defined line metrics",  OFFSET(y_align), AV_OPT_TYPE_CONST, {.i64=YA_FONT},     0, 0, FLAGS, .unit = "y_align"},

    {"timecode",        "set initial timecode",             OFFSET(tc_opt_string), AV_OPT_TYPE_STRING,   {.str=NULL}, 0, 0, FLAGS},
    {"tc24hmax",        "set 24 hours max (timecode only)", OFFSET(tc24hmax),      AV_OPT_TYPE_BOOL,     {.i64=0},    0, 1, FLAGS},
    {"timecode_rate",   "set rate (timecode only)",         OFFSET(tc_rate),       AV_OPT_TYPE_RATIONAL, {.dbl=0},    0, INT_MAX, FLAGS},
    {"r",               "set rate (timecode only)",         OFFSET(tc_rate),       AV_OPT_TYPE_RATIONAL, {.dbl=0},    0, INT_MAX, FLAGS},
    {"rate",            "set rate (timecode only)",         OFFSET(tc_rate),       AV_OPT_TYPE_RATIONAL, {.dbl=0},    0, INT_MAX, FLAGS},
    {"reload",          "reload text file at specified frame interval", OFFSET(reload), AV_OPT_TYPE_INT, {.i64=0},    0, INT_MAX, FLAGS},
    {"alpha",           "apply alpha while rendering",      OFFSET(a_expr),        AV_OPT_TYPE_STRING,   {.str = "1"}, .flags = TFLAGS},
    {"fix_bounds",      "check and fix text coords to avoid clipping", OFFSET(fix_bounds), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS},
    {"start_number",    "start frame number for n/frame_num variable", OFFSET(start_number), AV_OPT_TYPE_INT, {.i64=0}, 0, INT_MAX, FLAGS},
    {"text_source",     "the source of text", OFFSET(text_source_string), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 1, FLAGS },
    {"glyph_cache",     "directory for persistent glyph cache", OFFSET(glyph_cache_dir), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, FLAGS },

#if CONFIG_LIBFRIBIDI
    {"text_shaping", "attempt to shape text before drawing", OFFSET(text_shaping), AV_OPT_TYPE_BOOL, {.i64=1}, 0, 1, FLAGS},
#endif

    /* FT_LOAD_* flags */
    { "ft_load_flags", "set font loading flags for libfreetype", OFFSET(ft_load_flags), AV_OPT_TYPE_FLAGS, { .i64 = FT_LOAD_DEFAULT }, 0, INT_MAX, FLAGS, .unit = "ft_load_flags" },
        { "default",                     NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FT_LOAD_DEFAULT },                     .flags = FLAGS, .unit = "ft_load_flags" },
        { "no_scale",                    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FT_LOAD_NO_SCALE },                    .flags = FLAGS, .unit = "ft_load_flags" },
        { "no_hinting",                  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FT_LOAD_NO_HINTING },                  .flags = FLAGS, .unit = "ft_load_flags" },
        { "render",                      NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FT_LOAD_RENDER },                      .flags = FLAGS, .unit = "ft_load_flags" },
        { "no_bitmap",                   NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FT_LOAD_NO_BITMAP },                   .flags = FLAGS, .unit = "ft_load_flags" },
        { "vertical_layout",             NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FT_LOAD_VERTICAL_LAYOUT },             .flags = FLAGS, .unit = "ft_load_flags" },
        { "force_autohint",              NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FT_LOAD_FORCE_AUTOHINT },              .flags = FLAGS, .unit = "ft_load_flags" },
        { "crop_bitmap",                 NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FT_LOAD_CROP_BITMAP },                 .flags = FLAGS, .unit = "ft_load_flags" },
        { "pedantic",                    NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FT_LOAD_PEDANTIC },                    .flags = FLAGS, .unit = "ft_load_flags" },
        { "ignore_global_advance_width", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH }, .flags = FLAGS, .unit = "ft_load_flags" },
        { "no_recurse",                  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FT_LOAD_NO_RECURSE },                  .flags = FLAGS, .unit = "ft_load_flags" },
        { "ignore_transform",            NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FT_LOAD_IGNORE_TRANSFORM },            .flags = FLAGS, .unit = "ft_load_flags" },
        { "monochrome",                  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FT_LOAD_MONOCHROME },                  .flags = FLAGS, .unit = "ft_load_flags" },
        { "linear_design",               NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FT_LOAD_LINEAR_DESIGN },               .flags = FLAGS, .unit = "ft_load_flags" },
        { "no_autohint",                 NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FT_LOAD_NO_AUTOHINT },                 .flags = FLAGS, .unit = "ft_load_flags" },
    { NULL }
};

AVFILTER_DEFINE_CLASS(drawtext);

#undef __FTERRORS_H__
#define FT_ERROR_START_LIST {
#define FT_ERRORDEF(e, v, s) { (e), (s) },
#define FT_ERROR_END_LIST { 0, NULL } };

static const struct ft_error {
    int err;
    const char *err_msg;
} ft_errors[] =
#include FT_ERRORS_H

#define FT_ERRMSG(e) ft_errors[e].err_msg

/* Hash table functions for O(1) glyph lookup */
static inline uint32_t glyph_hash_func(uint32_t code, unsigned int fontsize)
{
    return (code * 31 + fontsize) % GLYPH_HASH_SIZE;
}

static Glyph *glyph_hash_find(GlyphHash *hash, uint32_t code, unsigned int fontsize)
{
    uint32_t idx = glyph_hash_func(code, fontsize);
    Glyph *g = hash->buckets[idx];
    while (g) {
        if (g->code == code && g->fontsize == fontsize)
            return g;
        g = g->next;
    }
    return NULL;
}

static void glyph_hash_insert(GlyphHash *hash, Glyph *glyph)
{
    uint32_t idx = glyph_hash_func(glyph->code, glyph->fontsize);
    glyph->next = hash->buckets[idx];
    hash->buckets[idx] = glyph;
}

/* Free a bitmap glyph, handling both FreeType-allocated and disk-cached */
static void free_bitmap_glyph(FT_BitmapGlyph bglyph, int from_cache)
{
    if (!bglyph)
        return;
    if (from_cache) {
        av_free(bglyph->bitmap.buffer);
        av_free(bglyph);
    } else {
        FT_Done_Glyph((FT_Glyph)bglyph);
    }
}

static void glyph_hash_free(GlyphHash *hash)
{
    for (int i = 0; i < GLYPH_HASH_SIZE; i++) {
        Glyph *g = hash->buckets[i];
        while (g) {
            Glyph *next = g->next;
            FT_Done_Glyph(g->glyph);
            if (g->border_glyph)
                FT_Done_Glyph(g->border_glyph);
            for (int t = 0; t < 16; t++) {
                free_bitmap_glyph(g->bglyph[t], (g->bglyph_from_cache >> t) & 1);
                free_bitmap_glyph(g->border_bglyph[t], (g->border_bglyph_from_cache >> t) & 1);
            }
            av_free(g);
            g = next;
        }
        hash->buckets[i] = NULL;
    }
}

static void glyph_hash_free_borders(GlyphHash *hash)
{
    for (int i = 0; i < GLYPH_HASH_SIZE; i++) {
        Glyph *g = hash->buckets[i];
        while (g) {
            if (g->border_glyph) {
                for (int t = 0; t < 16; t++) {
                    if (g->border_bglyph[t]) {
                        free_bitmap_glyph(g->border_bglyph[t], (g->border_bglyph_from_cache >> t) & 1);
                        g->border_bglyph[t] = NULL;
                    }
                }
                g->border_bglyph_from_cache = 0;
                FT_Done_Glyph(g->border_glyph);
                g->border_glyph = NULL;
            }
            g = g->next;
        }
    }
}

/*
 * Disk-based glyph cache functions
 *
 * Cache file format (version 1):
 *   - 4 bytes: magic "GCCH"
 *   - 4 bytes: version (1)
 *   - 4 bytes: bitmap width
 *   - 4 bytes: bitmap rows (height)
 *   - 4 bytes: bitmap pitch
 *   - 4 bytes: left offset (signed)
 *   - 4 bytes: top offset (signed)
 *   - N bytes: bitmap data (rows * pitch)
 */

#define GLYPH_CACHE_MAGIC 0x48434347  /* "GCCH" */
#define GLYPH_CACHE_VERSION 1

#define SHAPE_CACHE_MAGIC 0x48435348  /* "HSCH" - HarfBuzz Shape Cache */
#define SHAPE_CACHE_VERSION 1

#define WORD_CACHE_MAGIC 0x44524F57  /* "WORD" */
#define WORD_CACHE_VERSION 1

/* Compute a 64-bit FNV-1a hash of a file's contents */
static uint64_t compute_file_hash(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint64_t hash = 0xcbf29ce484222325ULL;  /* FNV offset basis */
    uint8_t buf[4096];
    size_t n;

    if (!f)
        return 0;

    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            hash ^= buf[i];
            hash *= 0x100000001b3ULL;  /* FNV prime */
        }
    }
    fclose(f);
    return hash;
}

/* Build cache file path: <cache_dir>/<font_hash>_<code>_<fontsize>_<subpixel>_<borderw>.gcch */
static int build_cache_path(char *buf, size_t bufsize, const char *cache_dir,
                            uint64_t font_hash, uint32_t code, unsigned int fontsize,
                            int subpixel_idx, int borderw, int is_border)
{
    int ret = snprintf(buf, bufsize, "%s/%016" PRIx64 "_%08x_%u_%d_%d_%d.gcch",
                       cache_dir, font_hash, code, fontsize, subpixel_idx, borderw, is_border);
    return (ret > 0 && (size_t)ret < bufsize) ? 0 : -1;
}

/* Write a bitmap glyph to disk cache */
static int disk_cache_write(const char *path, FT_BitmapGlyph bglyph)
{
    FILE *f;
    uint32_t header[7];
    FT_Bitmap *bmp = &bglyph->bitmap;

    f = fopen(path, "wb");
    if (!f)
        return -1;

    header[0] = GLYPH_CACHE_MAGIC;
    header[1] = GLYPH_CACHE_VERSION;
    header[2] = bmp->width;
    header[3] = bmp->rows;
    header[4] = bmp->pitch;
    header[5] = (uint32_t)bglyph->left;
    header[6] = (uint32_t)bglyph->top;

    if (fwrite(header, sizeof(header), 1, f) != 1) {
        fclose(f);
        return -1;
    }

    if (bmp->rows > 0 && bmp->pitch > 0) {
        size_t data_size = (size_t)bmp->rows * (size_t)bmp->pitch;
        if (fwrite(bmp->buffer, 1, data_size, f) != data_size) {
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    return 0;
}

/* Read a bitmap glyph from disk cache; returns a synthetic FT_BitmapGlyph or NULL */
static FT_BitmapGlyph disk_cache_read(FT_Library library, const char *path)
{
    FILE *f;
    uint32_t header[7];
    FT_BitmapGlyph bglyph = NULL;
    FT_Bitmap *bmp;
    size_t data_size;
    FT_Error err;

    f = fopen(path, "rb");
    if (!f)
        return NULL;

    if (fread(header, sizeof(header), 1, f) != 1)
        goto fail;

    if (header[0] != GLYPH_CACHE_MAGIC || header[1] != GLYPH_CACHE_VERSION)
        goto fail;

    /* Allocate a FT_GlyphRec structure for a bitmap glyph */
    bglyph = av_mallocz(sizeof(*bglyph));
    if (!bglyph)
        goto fail;

    bglyph->root.library = library;
    bglyph->root.format = FT_GLYPH_FORMAT_BITMAP;
    bglyph->left = (FT_Int)header[5];
    bglyph->top = (FT_Int)header[6];

    bmp = &bglyph->bitmap;
    bmp->width = header[2];
    bmp->rows = header[3];
    bmp->pitch = header[4];
    bmp->pixel_mode = FT_PIXEL_MODE_GRAY;
    bmp->num_grays = 256;

    data_size = (size_t)bmp->rows * (size_t)bmp->pitch;
    if (data_size > 0) {
        /* Use FreeType's allocator for the bitmap buffer */
        bmp->buffer = av_malloc(data_size);
        if (!bmp->buffer)
            goto fail;

        if (fread(bmp->buffer, 1, data_size, f) != data_size)
            goto fail;
    } else {
        bmp->buffer = NULL;
    }

    fclose(f);
    return bglyph;

fail:
    if (bglyph) {
        av_free(bmp->buffer);
        av_free(bglyph);
    }
    fclose(f);
    return NULL;
}

/* Try to load a cached bitmap glyph from disk */
static FT_BitmapGlyph disk_cache_try_load(AVFilterContext *ctx, DrawTextContext *s,
                                          uint32_t code, unsigned int fontsize,
                                          int subpixel_idx, int is_border)
{
    char path[512];
    FT_BitmapGlyph result;

    if (!s->glyph_cache_dir) {
        av_log(ctx, AV_LOG_DEBUG, "Glyph cache: miss code=0x%x size=%u subpx=%d border=%d reason=disabled\n",
               code, fontsize, subpixel_idx, is_border);
        return NULL;
    }

    if (!s->glyph_cache_initialized) {
        av_log(ctx, AV_LOG_DEBUG, "Glyph cache: miss code=0x%x size=%u subpx=%d border=%d reason=not_initialized\n",
               code, fontsize, subpixel_idx, is_border);
        return NULL;
    }

    if (build_cache_path(path, sizeof(path), s->glyph_cache_dir, s->font_hash,
                         code, fontsize, subpixel_idx, s->borderw, is_border) < 0) {
        av_log(ctx, AV_LOG_DEBUG, "Glyph cache: miss code=0x%x size=%u subpx=%d border=%d reason=path_too_long\n",
               code, fontsize, subpixel_idx, is_border);
        return NULL;
    }

    result = disk_cache_read(s->library, path);
    if (result) {
        av_log(ctx, AV_LOG_DEBUG, "Glyph cache: hit code=0x%x size=%u subpx=%d border=%d\n",
               code, fontsize, subpixel_idx, is_border);
    } else {
        av_log(ctx, AV_LOG_DEBUG, "Glyph cache: miss code=0x%x size=%u subpx=%d border=%d reason=file_not_found_or_invalid\n",
               code, fontsize, subpixel_idx, is_border);
    }
    return result;
}

/* Save a rendered bitmap glyph to disk cache */
static void disk_cache_save(AVFilterContext *ctx, DrawTextContext *s,
                            uint32_t code, unsigned int fontsize,
                            int subpixel_idx, int is_border, FT_BitmapGlyph bglyph)
{
    char path[512];
    int ret;

    if (!s->glyph_cache_dir || !s->glyph_cache_initialized)
        return;

    if (build_cache_path(path, sizeof(path), s->glyph_cache_dir, s->font_hash,
                         code, fontsize, subpixel_idx, s->borderw, is_border) < 0) {
        av_log(ctx, AV_LOG_DEBUG, "Glyph cache: save failed code=0x%x size=%u subpx=%d border=%d reason=path_too_long\n",
               code, fontsize, subpixel_idx, is_border);
        return;
    }

    ret = disk_cache_write(path, bglyph);
    if (ret < 0) {
        av_log(ctx, AV_LOG_DEBUG, "Glyph cache: save failed code=0x%x size=%u subpx=%d border=%d reason=write_error\n",
               code, fontsize, subpixel_idx, is_border);
    } else {
        av_log(ctx, AV_LOG_DEBUG, "Glyph cache: saved code=0x%x size=%u subpx=%d border=%d\n",
               code, fontsize, subpixel_idx, is_border);
    }
}

/* Compute a 64-bit FNV-1a hash of text content */
static uint64_t compute_text_hash(const char *text, int len)
{
    uint64_t hash = 0xcbf29ce484222325ULL;  /* FNV offset basis */
    for (int i = 0; i < len; i++) {
        hash ^= (unsigned char)text[i];
        hash *= 0x100000001b3ULL;  /* FNV prime */
    }
    return hash;
}

/* Build shaping cache file path: <cache_dir>/shape_<font_hash>_<text_hash>_<fontsize>.hbsc */
static int build_shape_cache_path(char *buf, size_t bufsize, const char *cache_dir,
                                  uint64_t font_hash, uint64_t text_hash, unsigned int fontsize)
{
    int ret = snprintf(buf, bufsize, "%s/shape_%016" PRIx64 "_%016" PRIx64 "_%u.hbsc",
                       cache_dir, font_hash, text_hash, fontsize);
    return (ret > 0 && (size_t)ret < bufsize) ? 0 : -1;
}

/**
 * Cached shaping data - stored separately from HarfbuzzData since
 * the hb_buffer_t pointers are not valid across runs.
 */
typedef struct ShapeCacheEntry {
    uint32_t glyph_count;
    uint32_t *codepoints;   /* glyph codepoints */
    uint32_t *clusters;     /* cluster indices */
    int32_t *x_advances;
    int32_t *y_advances;
    int32_t *x_offsets;
    int32_t *y_offsets;
} ShapeCacheEntry;

static void shape_cache_entry_free(ShapeCacheEntry *entry)
{
    if (entry) {
        av_freep(&entry->codepoints);
        av_freep(&entry->clusters);
        av_freep(&entry->x_advances);
        av_freep(&entry->y_advances);
        av_freep(&entry->x_offsets);
        av_freep(&entry->y_offsets);
        entry->glyph_count = 0;
    }
}

/* Write shaping results to disk cache */
static int shape_cache_write(const char *path, hb_glyph_info_t *info,
                             hb_glyph_position_t *pos, unsigned int glyph_count)
{
    FILE *f;
    uint32_t header[3];

    f = fopen(path, "wb");
    if (!f)
        return -1;

    header[0] = SHAPE_CACHE_MAGIC;
    header[1] = SHAPE_CACHE_VERSION;
    header[2] = glyph_count;

    if (fwrite(header, sizeof(header), 1, f) != 1) {
        fclose(f);
        return -1;
    }

    /* Write glyph data: codepoint, cluster, x_advance, y_advance, x_offset, y_offset */
    for (unsigned int i = 0; i < glyph_count; i++) {
        uint32_t glyph_data[2];
        int32_t pos_data[4];

        glyph_data[0] = info[i].codepoint;
        glyph_data[1] = info[i].cluster;
        pos_data[0] = pos[i].x_advance;
        pos_data[1] = pos[i].y_advance;
        pos_data[2] = pos[i].x_offset;
        pos_data[3] = pos[i].y_offset;

        if (fwrite(glyph_data, sizeof(glyph_data), 1, f) != 1 ||
            fwrite(pos_data, sizeof(pos_data), 1, f) != 1) {
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    return 0;
}

/* Read shaping results from disk cache; returns 0 on success, -1 on failure */
static int shape_cache_read(const char *path, ShapeCacheEntry *entry)
{
    FILE *f;
    uint32_t header[3];

    memset(entry, 0, sizeof(*entry));

    f = fopen(path, "rb");
    if (!f)
        return -1;

    if (fread(header, sizeof(header), 1, f) != 1)
        goto fail;

    if (header[0] != SHAPE_CACHE_MAGIC || header[1] != SHAPE_CACHE_VERSION)
        goto fail;

    entry->glyph_count = header[2];
    if (entry->glyph_count == 0) {
        fclose(f);
        return 0;
    }

    /* Allocate arrays */
    entry->codepoints = av_malloc_array(entry->glyph_count, sizeof(*entry->codepoints));
    entry->clusters = av_malloc_array(entry->glyph_count, sizeof(*entry->clusters));
    entry->x_advances = av_malloc_array(entry->glyph_count, sizeof(*entry->x_advances));
    entry->y_advances = av_malloc_array(entry->glyph_count, sizeof(*entry->y_advances));
    entry->x_offsets = av_malloc_array(entry->glyph_count, sizeof(*entry->x_offsets));
    entry->y_offsets = av_malloc_array(entry->glyph_count, sizeof(*entry->y_offsets));

    if (!entry->codepoints || !entry->clusters || !entry->x_advances ||
        !entry->y_advances || !entry->x_offsets || !entry->y_offsets)
        goto fail;

    /* Read glyph data */
    for (unsigned int i = 0; i < entry->glyph_count; i++) {
        uint32_t glyph_data[2];
        int32_t pos_data[4];

        if (fread(glyph_data, sizeof(glyph_data), 1, f) != 1 ||
            fread(pos_data, sizeof(pos_data), 1, f) != 1)
            goto fail;

        entry->codepoints[i] = glyph_data[0];
        entry->clusters[i] = glyph_data[1];
        entry->x_advances[i] = pos_data[0];
        entry->y_advances[i] = pos_data[1];
        entry->x_offsets[i] = pos_data[2];
        entry->y_offsets[i] = pos_data[3];
    }

    fclose(f);
    return 0;

fail:
    shape_cache_entry_free(entry);
    fclose(f);
    return -1;
}

/**
 * Build subset font cache path: <cache_dir>/subset_<font_hash>.ttf
 */
static int build_subset_cache_path(char *buf, size_t bufsize, const char *cache_dir,
                                   uint64_t font_hash)
{
    int ret = snprintf(buf, bufsize, "%s/subset_%016" PRIx64 ".ttf",
                       cache_dir, font_hash);
    return (ret > 0 && (size_t)ret < bufsize) ? 0 : -1;
}

/**
 * Collect all unique codepoints from text for subsetting.
 */
static hb_set_t *collect_codepoints_from_text(const char *text)
{
    hb_set_t *codepoints = hb_set_create();
    const uint8_t *p = (const uint8_t *)text;

    if (!codepoints)
        return NULL;

    while (*p) {
        hb_codepoint_t cp;

        /* Decode UTF-8 */
        if ((*p & 0x80) == 0) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0 && p[1]) {
            cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            cp = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            p += 4;
        } else {
            /* Invalid UTF-8, skip byte */
            p++;
            continue;
        }

        hb_set_add(codepoints, cp);
    }

    /* Always include space and common punctuation */
    hb_set_add(codepoints, ' ');
    hb_set_add(codepoints, '.');
    hb_set_add(codepoints, ',');
    hb_set_add(codepoints, ':');
    hb_set_add(codepoints, ';');
    hb_set_add(codepoints, '!');
    hb_set_add(codepoints, '?');
    hb_set_add(codepoints, '-');
    hb_set_add(codepoints, '\'');
    hb_set_add(codepoints, '"');
    hb_set_add(codepoints, '0');
    hb_set_add(codepoints, '1');
    hb_set_add(codepoints, '2');
    hb_set_add(codepoints, '3');
    hb_set_add(codepoints, '4');
    hb_set_add(codepoints, '5');
    hb_set_add(codepoints, '6');
    hb_set_add(codepoints, '7');
    hb_set_add(codepoints, '8');
    hb_set_add(codepoints, '9');

    return codepoints;
}

/**
 * Create a subset font containing only the needed codepoints.
 * Returns 0 on success, negative on failure.
 */
static int create_subset_font(AVFilterContext *ctx, const char *input_path,
                              const char *output_path, const char *text)
{
    hb_blob_t *input_blob = NULL;
    hb_face_t *input_face = NULL;
    hb_subset_input_t *input = NULL;
    hb_face_t *subset_face = NULL;
    hb_blob_t *subset_blob = NULL;
    hb_set_t *codepoints = NULL;
    FILE *f = NULL;
    const char *data;
    unsigned int length;
    int ret = -1;

    /* Load input font with mmap */
    input_blob = hb_blob_create_from_file_or_fail(input_path);
    if (!input_blob) {
        av_log(ctx, AV_LOG_WARNING, "Font subset: could not load font file '%s'\n", input_path);
        goto cleanup;
    }

    input_face = hb_face_create(input_blob, 0);
    if (!input_face || hb_face_get_glyph_count(input_face) == 0) {
        av_log(ctx, AV_LOG_WARNING, "Font subset: could not create face from '%s'\n", input_path);
        goto cleanup;
    }

    /* Collect codepoints from text */
    codepoints = collect_codepoints_from_text(text);
    if (!codepoints) {
        av_log(ctx, AV_LOG_WARNING, "Font subset: could not collect codepoints\n");
        goto cleanup;
    }

    av_log(ctx, AV_LOG_DEBUG, "Font subset: collected %u unique codepoints from text\n",
           hb_set_get_population(codepoints));

    /* Create subset input */
    input = hb_subset_input_create_or_fail();
    if (!input) {
        av_log(ctx, AV_LOG_WARNING, "Font subset: could not create subset input\n");
        goto cleanup;
    }

    /* Set codepoints to keep */
    hb_set_union(hb_subset_input_unicode_set(input), codepoints);

    /* Keep all OpenType features for proper shaping */
    hb_set_clear(hb_subset_input_set(input, HB_SUBSET_SETS_DROP_TABLE_TAG));

    /* Create subset */
    subset_face = hb_subset_or_fail(input_face, input);
    if (!subset_face) {
        av_log(ctx, AV_LOG_WARNING, "Font subset: subsetting failed\n");
        goto cleanup;
    }

    /* Get subset blob */
    subset_blob = hb_face_reference_blob(subset_face);
    if (!subset_blob) {
        av_log(ctx, AV_LOG_WARNING, "Font subset: could not get subset blob\n");
        goto cleanup;
    }

    data = hb_blob_get_data(subset_blob, &length);
    if (!data || length == 0) {
        av_log(ctx, AV_LOG_WARNING, "Font subset: empty subset data\n");
        goto cleanup;
    }

    /* Write to file */
    f = fopen(output_path, "wb");
    if (!f) {
        av_log(ctx, AV_LOG_WARNING, "Font subset: could not create output file '%s'\n", output_path);
        goto cleanup;
    }

    if (fwrite(data, 1, length, f) != length) {
        av_log(ctx, AV_LOG_WARNING, "Font subset: write error\n");
        fclose(f);
        goto cleanup;
    }

    fclose(f);
    f = NULL;

    av_log(ctx, AV_LOG_INFO, "Font subset: created %s (%u bytes, %u codepoints)\n",
           output_path, length, hb_set_get_population(codepoints));
    ret = 0;

cleanup:
    if (subset_blob) hb_blob_destroy(subset_blob);
    if (subset_face) hb_face_destroy(subset_face);
    if (input) hb_subset_input_destroy(input);
    if (codepoints) hb_set_destroy(codepoints);
    if (input_face) hb_face_destroy(input_face);
    if (input_blob) hb_blob_destroy(input_blob);
    return ret;
}

/**
 * Create HarfBuzz font using memory-mapped blob for faster loading.
 * Optionally uses a cached subset font if available.
 */
static int create_hb_font_mmap(AVFilterContext *ctx, DrawTextContext *s)
{
    const char *font_path = s->font_path;
    char subset_path[512];
    int use_subset = 0;

    /* Check for cached subset font */
    if (s->glyph_cache_dir && s->glyph_cache_initialized && s->text) {
        if (build_subset_cache_path(subset_path, sizeof(subset_path),
                                    s->glyph_cache_dir, s->font_hash) == 0) {
            struct stat st;
            if (stat(subset_path, &st) == 0 && S_ISREG(st.st_mode)) {
                /* Subset font exists, use it */
                font_path = subset_path;
                use_subset = 1;
                av_log(ctx, AV_LOG_DEBUG, "HarfBuzz: using cached subset font '%s'\n", subset_path);
            } else {
                /* Create subset font for future runs */
                if (create_subset_font(ctx, s->font_path, subset_path, s->text) == 0) {
                    font_path = subset_path;
                    use_subset = 1;
                }
            }
        }
    }

    /* Create memory-mapped blob */
    s->hb_blob = hb_blob_create_from_file_or_fail(font_path);
    if (!s->hb_blob) {
        av_log(ctx, AV_LOG_WARNING, "HarfBuzz: could not mmap font '%s', falling back to FreeType\n",
               font_path);
        /* Fall back to FreeType-based HarfBuzz font */
        s->hb_font = hb_ft_font_create_referenced(s->face);
        return s->hb_font ? 0 : AVERROR(ENOMEM);
    }

    av_log(ctx, AV_LOG_DEBUG, "HarfBuzz: loaded font via mmap (%s, %u bytes)\n",
           use_subset ? "subset" : "full",
           hb_blob_get_length(s->hb_blob));

    /* Create face from blob */
    s->hb_face = hb_face_create(s->hb_blob, 0);
    if (!s->hb_face || hb_face_get_glyph_count(s->hb_face) == 0) {
        av_log(ctx, AV_LOG_WARNING, "HarfBuzz: could not create face from blob, falling back to FreeType\n");
        hb_blob_destroy(s->hb_blob);
        s->hb_blob = NULL;
        s->hb_font = hb_ft_font_create_referenced(s->face);
        return s->hb_font ? 0 : AVERROR(ENOMEM);
    }

    /* Create font from face */
    s->hb_font = hb_font_create(s->hb_face);
    if (!s->hb_font) {
        hb_face_destroy(s->hb_face);
        hb_blob_destroy(s->hb_blob);
        s->hb_face = NULL;
        s->hb_blob = NULL;
        return AVERROR(ENOMEM);
    }

    /* Set scale to match FreeType (26.6 fixed point) */
    hb_font_set_scale(s->hb_font, s->fontsize * 64, s->fontsize * 64);

    /* Enable OpenType features */
    hb_ot_font_set_funcs(s->hb_font);

    return 0;
}

/**
 * Cached rendered word bitmap.
 * Contains the pre-composited grayscale bitmap for a word,
 * allowing us to skip shaping and glyph rendering for repeated words.
 */
typedef struct WordBitmap {
    uint8_t *buffer;        ///< grayscale bitmap data
    int width;              ///< bitmap width in pixels
    int height;             ///< bitmap height in pixels
    int pitch;              ///< bytes per row
    int left;               ///< left bearing (x offset from origin)
    int top;                ///< top bearing (y offset from baseline)
    int advance;            ///< horizontal advance to next word
} WordBitmap;

/** Entry in the in-memory word bitmap cache */
typedef struct WordCacheEntry {
    uint64_t text_hash;     ///< hash of word text
    uint64_t style_hash;    ///< hash of style parameters (fontsize, colors, etc)
    WordBitmap bitmap;      ///< cached rendered bitmap
    struct WordCacheEntry *next;  ///< next entry in hash bucket chain
} WordCacheEntry;

/**
 * Compute a hash of the rendering style parameters.
 * Used as part of the word cache key to ensure style changes invalidate cache.
 */
static uint64_t compute_style_hash(unsigned int fontsize, int borderw,
                                   const uint8_t *fontcolor_rgba,
                                   const uint8_t *bordercolor_rgba)
{
    uint64_t hash = 0xcbf29ce484222325ULL;  /* FNV offset basis */
    uint8_t buf[16];

    /* Pack style params into a buffer */
    buf[0] = (fontsize >> 0) & 0xFF;
    buf[1] = (fontsize >> 8) & 0xFF;
    buf[2] = (fontsize >> 16) & 0xFF;
    buf[3] = (fontsize >> 24) & 0xFF;
    buf[4] = (borderw >> 0) & 0xFF;
    buf[5] = (borderw >> 8) & 0xFF;
    buf[6] = fontcolor_rgba[0];
    buf[7] = fontcolor_rgba[1];
    buf[8] = fontcolor_rgba[2];
    buf[9] = fontcolor_rgba[3];
    buf[10] = bordercolor_rgba[0];
    buf[11] = bordercolor_rgba[1];
    buf[12] = bordercolor_rgba[2];
    buf[13] = bordercolor_rgba[3];
    buf[14] = 0;
    buf[15] = 0;

    for (int i = 0; i < 16; i++) {
        hash ^= buf[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

/**
 * Build word cache path: <cache_dir>/word_<font_hash>_<style_hash>_<text_hash>.wcch
 */
static int build_word_cache_path(char *buf, size_t bufsize, const char *cache_dir,
                                 uint64_t font_hash, uint64_t style_hash, uint64_t text_hash)
{
    int ret = snprintf(buf, bufsize, "%s/word_%016" PRIx64 "_%016" PRIx64 "_%016" PRIx64 ".wcch",
                       cache_dir, font_hash, style_hash, text_hash);
    return (ret > 0 && (size_t)ret < bufsize) ? 0 : -1;
}

/**
 * Write a rendered word bitmap to disk cache.
 * Format: magic(4) | version(4) | width(4) | height(4) | pitch(4) | left(4) | top(4) | advance(4) | data
 */
static int word_cache_write(const char *path, const WordBitmap *word)
{
    FILE *f;
    uint32_t header[8];
    size_t data_size;

    f = fopen(path, "wb");
    if (!f)
        return -1;

    header[0] = WORD_CACHE_MAGIC;
    header[1] = WORD_CACHE_VERSION;
    header[2] = word->width;
    header[3] = word->height;
    header[4] = word->pitch;
    header[5] = (uint32_t)word->left;
    header[6] = (uint32_t)word->top;
    header[7] = (uint32_t)word->advance;

    if (fwrite(header, sizeof(header), 1, f) != 1) {
        fclose(f);
        return -1;
    }

    data_size = (size_t)word->height * (size_t)word->pitch;
    if (data_size > 0 && word->buffer) {
        if (fwrite(word->buffer, 1, data_size, f) != data_size) {
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    return 0;
}

/**
 * Read a rendered word bitmap from disk cache.
 * Returns 0 on success, -1 on failure.
 */
static int word_cache_read(const char *path, WordBitmap *word)
{
    FILE *f;
    uint32_t header[8];
    size_t data_size;

    memset(word, 0, sizeof(*word));

    f = fopen(path, "rb");
    if (!f)
        return -1;

    if (fread(header, sizeof(header), 1, f) != 1)
        goto fail;

    if (header[0] != WORD_CACHE_MAGIC || header[1] != WORD_CACHE_VERSION)
        goto fail;

    word->width = header[2];
    word->height = header[3];
    word->pitch = header[4];
    word->left = (int32_t)header[5];
    word->top = (int32_t)header[6];
    word->advance = (int32_t)header[7];

    data_size = (size_t)word->height * (size_t)word->pitch;
    if (data_size > 0) {
        word->buffer = av_malloc(data_size);
        if (!word->buffer)
            goto fail;

        if (fread(word->buffer, 1, data_size, f) != data_size)
            goto fail;
    }

    fclose(f);
    return 0;

fail:
    av_freep(&word->buffer);
    memset(word, 0, sizeof(*word));
    fclose(f);
    return -1;
}

static void word_bitmap_free(WordBitmap *word)
{
    if (word) {
        av_freep(&word->buffer);
        memset(word, 0, sizeof(*word));
    }
}

/* ======================== In-memory word cache ======================== */

static inline uint32_t word_cache_hash_func(uint64_t text_hash, uint64_t style_hash)
{
    /* Combine both hashes and map to bucket index */
    return (uint32_t)((text_hash ^ style_hash) % WORD_CACHE_HASH_SIZE);
}

/**
 * Find a word bitmap in the in-memory cache.
 * Returns pointer to cached entry, or NULL if not found.
 */
static WordCacheEntry *word_mem_cache_find(WordCacheHash *cache,
                                           uint64_t text_hash, uint64_t style_hash)
{
    uint32_t idx = word_cache_hash_func(text_hash, style_hash);
    WordCacheEntry *e = cache->buckets[idx];
    while (e) {
        if (e->text_hash == text_hash && e->style_hash == style_hash)
            return e;
        e = e->next;
    }
    return NULL;
}

/**
 * Insert a word bitmap into the in-memory cache.
 * Takes ownership of the bitmap buffer.
 */
static void word_mem_cache_insert(WordCacheHash *cache,
                                  uint64_t text_hash, uint64_t style_hash,
                                  const WordBitmap *bitmap)
{
    WordCacheEntry *entry;
    uint32_t idx;

    if (!bitmap || !bitmap->buffer)
        return;

    /* Check if already exists */
    if (word_mem_cache_find(cache, text_hash, style_hash))
        return;

    entry = av_mallocz(sizeof(*entry));
    if (!entry)
        return;

    entry->text_hash = text_hash;
    entry->style_hash = style_hash;

    /* Copy the bitmap data */
    entry->bitmap.width = bitmap->width;
    entry->bitmap.height = bitmap->height;
    entry->bitmap.pitch = bitmap->pitch;
    entry->bitmap.left = bitmap->left;
    entry->bitmap.top = bitmap->top;
    entry->bitmap.advance = bitmap->advance;

    size_t buf_size = (size_t)bitmap->height * bitmap->pitch;
    entry->bitmap.buffer = av_malloc(buf_size);
    if (!entry->bitmap.buffer) {
        av_free(entry);
        return;
    }
    memcpy(entry->bitmap.buffer, bitmap->buffer, buf_size);

    /* Insert at head of bucket */
    idx = word_cache_hash_func(text_hash, style_hash);
    entry->next = cache->buckets[idx];
    cache->buckets[idx] = entry;

    cache->entry_count++;
    cache->total_bytes += buf_size;
}

/**
 * Free all entries in the in-memory word cache.
 */
static void word_mem_cache_free(WordCacheHash *cache)
{
    for (int i = 0; i < WORD_CACHE_HASH_SIZE; i++) {
        WordCacheEntry *e = cache->buckets[i];
        while (e) {
            WordCacheEntry *next = e->next;
            av_freep(&e->bitmap.buffer);
            av_free(e);
            e = next;
        }
        cache->buckets[i] = NULL;
    }
    cache->entry_count = 0;
    cache->total_bytes = 0;
}

/**
 * Try to load a cached word/line bitmap from disk.
 * Returns 0 on success (cache hit), -1 on miss/failure.
 */
static int word_cache_try_load(AVFilterContext *ctx, DrawTextContext *s,
                               const char *text, int text_len,
                               int borderw, WordBitmap *word)
{
    char path[512];
    uint64_t text_hash, style_hash;
    WordCacheEntry *mem_entry;

    text_hash = compute_text_hash(text, text_len);
    style_hash = compute_style_hash(s->fontsize, borderw,
                                    s->fontcolor.rgba, s->bordercolor.rgba);

    /* Check in-memory cache first (fastest) */
    mem_entry = word_mem_cache_find(&s->word_cache, text_hash, style_hash);
    if (mem_entry && mem_entry->bitmap.buffer) {
        /* Copy bitmap data to output */
        word->width = mem_entry->bitmap.width;
        word->height = mem_entry->bitmap.height;
        word->pitch = mem_entry->bitmap.pitch;
        word->left = mem_entry->bitmap.left;
        word->top = mem_entry->bitmap.top;
        word->advance = mem_entry->bitmap.advance;

        size_t buf_size = (size_t)word->height * word->pitch;
        word->buffer = av_malloc(buf_size);
        if (!word->buffer)
            return -1;
        memcpy(word->buffer, mem_entry->bitmap.buffer, buf_size);

        av_log(ctx, AV_LOG_DEBUG, "Word cache: memory hit text_hash=%016" PRIx64 " size=%dx%d\n",
               text_hash, word->width, word->height);
        return 0;
    }

    /* Check disk cache */
    if (!s->glyph_cache_dir || !s->glyph_cache_initialized)
        return -1;

    if (build_word_cache_path(path, sizeof(path), s->glyph_cache_dir,
                              s->font_hash, style_hash, text_hash) != 0)
        return -1;

    if (word_cache_read(path, word) == 0) {
        av_log(ctx, AV_LOG_DEBUG, "Word cache: disk hit text_hash=%016" PRIx64 " size=%dx%d\n",
               text_hash, word->width, word->height);
        /* Promote to in-memory cache for faster future access */
        word_mem_cache_insert(&s->word_cache, text_hash, style_hash, word);
        return 0;
    }

    return -1;
}

/**
 * Save a rendered word/line bitmap to disk cache.
 */
static void word_cache_save(AVFilterContext *ctx, DrawTextContext *s,
                            const char *text, int text_len,
                            int borderw, const WordBitmap *word)
{
    char path[512];
    uint64_t text_hash, style_hash;

    text_hash = compute_text_hash(text, text_len);
    style_hash = compute_style_hash(s->fontsize, borderw,
                                    s->fontcolor.rgba, s->bordercolor.rgba);

    /* Always add to in-memory cache */
    word_mem_cache_insert(&s->word_cache, text_hash, style_hash, word);
    av_log(ctx, AV_LOG_DEBUG, "Word cache: memory store text_hash=%016" PRIx64 " size=%dx%d (total=%d entries, %zu bytes)\n",
           text_hash, word->width, word->height,
           s->word_cache.entry_count, s->word_cache.total_bytes);

    /* Also save to disk cache if enabled */
    if (!s->glyph_cache_dir || !s->glyph_cache_initialized)
        return;

    if (build_word_cache_path(path, sizeof(path), s->glyph_cache_dir,
                              s->font_hash, style_hash, text_hash) != 0)
        return;

    if (word_cache_write(path, word) == 0) {
        av_log(ctx, AV_LOG_DEBUG, "Word cache: disk store text_hash=%016" PRIx64 " size=%dx%d (%d bytes)\n",
               text_hash, word->width, word->height,
               word->height * word->pitch);
    }
}

static void hb_destroy(HarfbuzzData *hb);
static void harfbuzz_warmup(hb_font_t *hb_font);
static inline int get_subpixel_idx(int shift_x64, int shift_y64);

/**
 * Render a range of glyphs to a WordBitmap by compositing their bitmaps.
 * The bitmap is grayscale (8-bit alpha mask).
 *
 * @param s         DrawText context
 * @param glyphs    Array of glyph info
 * @param start     Start index in glyphs array
 * @param count     Number of glyphs to render
 * @param x_offset  X offset to subtract from glyph positions (for word-relative coords)
 * @param borderw   Border width (0 for regular, >0 for border glyphs)
 * @param out       Output bitmap
 * @return 0 on success, negative on failure
 */
static int render_glyphs_to_bitmap(DrawTextContext *s, GlyphInfo *glyphs,
                                   int start, int count, int x_offset,
                                   int borderw, WordBitmap *out)
{
    int min_x = INT_MAX, max_x = INT_MIN;
    int min_y = INT_MAX, max_y = INT_MIN;
    int g, idx, x, y;
    Glyph *glyph;
    FT_BitmapGlyph b_glyph;
    GlyphInfo *info;
    int advance = 0;

    memset(out, 0, sizeof(*out));

    if (count <= 0)
        return 0;

    /* First pass: compute bounding box */
    for (g = start; g < start + count; g++) {
        info = &glyphs[g];
        glyph = glyph_hash_find(&s->glyph_hash, info->code, s->fontsize);
        if (!glyph)
            continue;

        idx = get_subpixel_idx(info->shift_x64, info->shift_y64);
        b_glyph = borderw ? glyph->border_bglyph[idx] : glyph->bglyph[idx];
        if (!b_glyph)
            continue;

        x = info->x - x_offset + b_glyph->left;
        y = info->y - b_glyph->top;

        min_x = FFMIN(min_x, x);
        max_x = FFMAX(max_x, x + (int)b_glyph->bitmap.width);
        min_y = FFMIN(min_y, y);
        max_y = FFMAX(max_y, y + (int)b_glyph->bitmap.rows);
    }

    /* Calculate advance (distance to next word) */
    if (count > 0) {
        GlyphInfo *last = &glyphs[start + count - 1];
        GlyphInfo *first = &glyphs[start];
        advance = last->x - first->x;
        /* Add the width of the last glyph */
        glyph = glyph_hash_find(&s->glyph_hash, last->code, s->fontsize);
        if (glyph) {
            idx = get_subpixel_idx(last->shift_x64, last->shift_y64);
            b_glyph = borderw ? glyph->border_bglyph[idx] : glyph->bglyph[idx];
            if (b_glyph)
                advance += b_glyph->bitmap.width + b_glyph->left;
        }
    }

    if (min_x >= max_x || min_y >= max_y)
        return 0;  /* Empty word */

    out->width = max_x - min_x;
    out->height = max_y - min_y;
    out->pitch = out->width;  /* 8-bit grayscale, no padding */
    out->left = min_x;
    out->top = -min_y;  /* Convert to top bearing */
    out->advance = advance;

    out->buffer = av_mallocz((size_t)out->height * out->pitch);
    if (!out->buffer)
        return AVERROR(ENOMEM);

    /* Second pass: composite glyphs into bitmap */
    for (g = start; g < start + count; g++) {
        info = &glyphs[g];
        glyph = glyph_hash_find(&s->glyph_hash, info->code, s->fontsize);
        if (!glyph)
            continue;

        idx = get_subpixel_idx(info->shift_x64, info->shift_y64);
        b_glyph = borderw ? glyph->border_bglyph[idx] : glyph->bglyph[idx];
        if (!b_glyph || b_glyph->bitmap.width == 0 || b_glyph->bitmap.rows == 0)
            continue;

        int gx = info->x - x_offset + b_glyph->left - min_x;
        int gy = info->y - b_glyph->top - min_y;
        FT_Bitmap *bmp = &b_glyph->bitmap;

        /* Composite glyph bitmap using max blending */
        for (int row = 0; row < (int)bmp->rows; row++) {
            if (gy + row < 0 || gy + row >= out->height)
                continue;
            uint8_t *dst = out->buffer + (gy + row) * out->pitch + gx;
            uint8_t *src = bmp->buffer + row * bmp->pitch;
            for (int col = 0; col < (int)bmp->width; col++) {
                if (gx + col < 0 || gx + col >= out->width)
                    continue;
                /* Max blending for overlapping glyphs */
                if (src[col] > dst[col])
                    dst[col] = src[col];
            }
        }
    }

    return 0;
}

static int glyph_cmp(const void *key, const void *b)
{
    const Glyph *a = key, *bb = b;
    int64_t diff = (int64_t)a->code - (int64_t)bb->code;

    if (diff != 0)
        return diff > 0 ? 1 : -1;
    else
        return FFDIFFSIGN((int64_t)a->fontsize, (int64_t)bb->fontsize);
}

static av_cold int set_fontsize(AVFilterContext *ctx, unsigned int fontsize)
{
    int err;
    DrawTextContext *s = ctx->priv;

    if ((err = FT_Set_Pixel_Sizes(s->face, 0, fontsize))) {
        av_log(ctx, AV_LOG_ERROR, "Could not set font size to %d pixels: %s\n",
               fontsize, FT_ERRMSG(err));
        return AVERROR(EINVAL);
    }

    // Whenever the underlying FT_Face changes, HarfBuzz has to be notified.
    if (s->hb_font) {
        if (s->hb_blob) {
            /* Using mmap-based font - update scale directly */
            hb_font_set_scale(s->hb_font, fontsize * 64, fontsize * 64);
        } else {
            /* Using FreeType-based font */
            hb_ft_font_changed(s->hb_font);
        }
    }

    s->fontsize = fontsize;

    return 0;
}

static av_cold int parse_fontsize(AVFilterContext *ctx)
{
    DrawTextContext *s = ctx->priv;
    int err;

    if (s->fontsize_pexpr)
        return 0;

    if (s->fontsize_expr == NULL)
        return AVERROR(EINVAL);

    if ((err = av_expr_parse(&s->fontsize_pexpr, s->fontsize_expr, var_names,
                             NULL, NULL, fun2_names, fun2, 0, ctx)) < 0)
        return err;

    return 0;
}

static av_cold int update_fontsize(AVFilterContext *ctx)
{
    DrawTextContext *s = ctx->priv;
    unsigned int fontsize = s->default_fontsize;
    int err;
    double size, roundedsize;

    // if no fontsize specified use the default
    if (s->fontsize_expr != NULL) {
        if ((err = parse_fontsize(ctx)) < 0)
           return err;

        size = av_expr_eval(s->fontsize_pexpr, s->var_values, &s->prng);
        if (!isnan(size)) {
            roundedsize = round(size);
            // test for overflow before cast
            if (!(roundedsize > INT_MIN && roundedsize < INT_MAX)) {
                av_log(ctx, AV_LOG_ERROR, "fontsize overflow\n");
                return AVERROR(EINVAL);
            }
            fontsize = roundedsize;
        }
    }

    if (fontsize == 0)
        fontsize = 1;

    // no change
    if (fontsize == s->fontsize)
        return 0;

    return set_fontsize(ctx, fontsize);
}

static int load_font_file(AVFilterContext *ctx, const char *path, int index)
{
    DrawTextContext *s = ctx->priv;
    int err;

    err = FT_New_Face(s->library, path, index, &s->face);
    if (err) {
#if !CONFIG_LIBFONTCONFIG
        av_log(ctx, AV_LOG_ERROR, "Could not load font \"%s\": %s\n",
               s->fontfile, FT_ERRMSG(err));
#endif
        return AVERROR(EINVAL);
    }

    /* Save resolved font path for disk cache keying */
    av_freep(&s->font_path);
    s->font_path = av_strdup(path);

    return 0;
}

#if CONFIG_LIBFONTCONFIG
static int load_font_fontconfig(AVFilterContext *ctx)
{
    DrawTextContext *s = ctx->priv;
    FcConfig *fontconfig;
    FcPattern *pat, *best;
    FcResult result = FcResultMatch;
    FcChar8 *filename;
    int index;
    double size;
    int err = AVERROR(ENOENT);
    int parse_err;

    fontconfig = FcInitLoadConfigAndFonts();
    if (!fontconfig) {
        av_log(ctx, AV_LOG_ERROR, "impossible to init fontconfig\n");
        return AVERROR_UNKNOWN;
    }
    pat = FcNameParse(s->fontfile ? s->fontfile :
                          (uint8_t *)(intptr_t)"default");
    if (!pat) {
        av_log(ctx, AV_LOG_ERROR, "could not parse fontconfig pat");
        return AVERROR(EINVAL);
    }

    FcPatternAddString(pat, FC_FAMILY, s->font);

    parse_err = parse_fontsize(ctx);
    if (!parse_err) {
        double size = av_expr_eval(s->fontsize_pexpr, s->var_values, &s->prng);

        if (isnan(size)) {
            av_log(ctx, AV_LOG_ERROR, "impossible to find font information");
            return AVERROR(EINVAL);
        }

        FcPatternAddDouble(pat, FC_SIZE, size);
    }

    FcDefaultSubstitute(pat);

    if (!FcConfigSubstitute(fontconfig, pat, FcMatchPattern)) {
        av_log(ctx, AV_LOG_ERROR, "could not substitute fontconfig options"); /* very unlikely */
        FcPatternDestroy(pat);
        return AVERROR(ENOMEM);
    }

    best = FcFontMatch(fontconfig, pat, &result);
    FcPatternDestroy(pat);

    if (!best || result != FcResultMatch) {
        av_log(ctx, AV_LOG_ERROR,
               "Cannot find a valid font for the family %s\n",
               s->font);
        goto fail;
    }

    if (
        FcPatternGetInteger(best, FC_INDEX, 0, &index   ) != FcResultMatch ||
        FcPatternGetDouble (best, FC_SIZE,  0, &size    ) != FcResultMatch) {
        av_log(ctx, AV_LOG_ERROR, "impossible to find font information");
        return AVERROR(EINVAL);
    }

    if (FcPatternGetString(best, FC_FILE, 0, &filename) != FcResultMatch) {
        av_log(ctx, AV_LOG_ERROR, "No file path for %s\n",
               s->font);
        goto fail;
    }

    av_log(ctx, AV_LOG_VERBOSE, "Using \"%s\"\n", filename);
    if (parse_err)
        s->default_fontsize = size + 0.5;

    err = load_font_file(ctx, filename, index);
    if (err)
        return err;
    FcConfigDestroy(fontconfig);
fail:
    FcPatternDestroy(best);
    return err;
}
#endif

static int load_font(AVFilterContext *ctx)
{
    DrawTextContext *s = ctx->priv;
    int err;

    /* load the face, and set up the encoding, which is by default UTF-8 */
    err = load_font_file(ctx, s->fontfile, 0);
    if (!err)
        return 0;
#if CONFIG_LIBFONTCONFIG
    err = load_font_fontconfig(ctx);
    if (!err)
        return 0;
#endif
    return err;
}

#if CONFIG_LIBFRIBIDI
static int shape_text(AVFilterContext *ctx)
{
    DrawTextContext *s = ctx->priv;
    uint8_t *tmp;
    int ret = AVERROR(ENOMEM);
    static const FriBidiFlags flags = FRIBIDI_FLAGS_DEFAULT |
                                      FRIBIDI_FLAGS_ARABIC;
    FriBidiChar *unicodestr = NULL;
    FriBidiStrIndex len;
    FriBidiParType direction = FRIBIDI_PAR_LTR;
    FriBidiStrIndex line_start = 0;
    FriBidiStrIndex line_end = 0;
    FriBidiLevel *embedding_levels = NULL;
    FriBidiArabicProp *ar_props = NULL;
    FriBidiCharType *bidi_types = NULL;
    FriBidiStrIndex i,j;

    len = strlen(s->text);
    if (!(unicodestr = av_malloc_array(len, sizeof(*unicodestr)))) {
        goto out;
    }
    len = fribidi_charset_to_unicode(FRIBIDI_CHAR_SET_UTF8,
                                     s->text, len, unicodestr);

    bidi_types = av_malloc_array(len, sizeof(*bidi_types));
    if (!bidi_types) {
        goto out;
    }

    fribidi_get_bidi_types(unicodestr, len, bidi_types);

    embedding_levels = av_malloc_array(len, sizeof(*embedding_levels));
    if (!embedding_levels) {
        goto out;
    }

    if (!fribidi_get_par_embedding_levels(bidi_types, len, &direction,
                                          embedding_levels)) {
        goto out;
    }

    ar_props = av_malloc_array(len, sizeof(*ar_props));
    if (!ar_props) {
        goto out;
    }

    fribidi_get_joining_types(unicodestr, len, ar_props);
    fribidi_join_arabic(bidi_types, len, embedding_levels, ar_props);
    fribidi_shape(flags, embedding_levels, len, ar_props, unicodestr);

    for (line_end = 0, line_start = 0; line_end < len; line_end++) {
        if (ff_is_newline(unicodestr[line_end]) || line_end == len - 1) {
            if (!fribidi_reorder_line(flags, bidi_types,
                                      line_end - line_start + 1, line_start,
                                      direction, embedding_levels, unicodestr,
                                      NULL)) {
                goto out;
            }
            line_start = line_end + 1;
        }
    }

    /* Remove zero-width fill chars put in by libfribidi */
    for (i = 0, j = 0; i < len; i++)
        if (unicodestr[i] != FRIBIDI_CHAR_FILL)
            unicodestr[j++] = unicodestr[i];
    len = j;

    if (!(tmp = av_realloc(s->text, (len * 4 + 1) * sizeof(*s->text)))) {
        /* Use len * 4, as a unicode character can be up to 4 bytes in UTF-8 */
        goto out;
    }

    s->text = tmp;
    len = fribidi_unicode_to_charset(FRIBIDI_CHAR_SET_UTF8,
                                     unicodestr, len, s->text);

    ret = 0;

out:
    av_free(unicodestr);
    av_free(embedding_levels);
    av_free(ar_props);
    av_free(bidi_types);
    return ret;
}
#endif

static enum AVFrameSideDataType text_source_string_parse(const char *text_source_string)
{
    av_assert0(text_source_string);
    if (!strcmp(text_source_string, "side_data_detection_bboxes")) {
        return AV_FRAME_DATA_DETECTION_BBOXES;
    } else {
        return AVERROR(EINVAL);
    }
}

static inline int get_subpixel_idx(int shift_x64, int shift_y64)
{
    int idx = (shift_x64 >> 2) + (shift_y64 >> 4);
    return idx;
}

// Loads and (optionally) renders a glyph
static int load_glyph(AVFilterContext *ctx, Glyph **glyph_ptr, uint32_t code, int8_t shift_x64, int8_t shift_y64)
{
    DrawTextContext *s = ctx->priv;
    Glyph *glyph;
    FT_Vector shift;
    int ret = 0;

    /* get glyph from hash table - O(1) lookup */
    glyph = glyph_hash_find(&s->glyph_hash, code, s->fontsize);
    if (!glyph) {
        if (FT_Load_Glyph(s->face, code, s->ft_load_flags)) {
            return AVERROR(EINVAL);
        }
        glyph = av_mallocz(sizeof(*glyph));
        if (!glyph) {
            ret = AVERROR(ENOMEM);
            goto error;
        }
        glyph->code  = code;
        glyph->fontsize = s->fontsize;
        if (FT_Get_Glyph(s->face->glyph, &glyph->glyph)) {
            ret = AVERROR(EINVAL);
            goto error;
        }
        if (s->borderw) {
            glyph->border_glyph = glyph->glyph;
            if (FT_Glyph_StrokeBorder(&glyph->border_glyph, s->stroker, 0, 0)) {
                ret = AVERROR_EXTERNAL;
                goto error;
            }
        }
        /* measure text height to calculate text_height (or the maximum text height) */
        FT_Glyph_Get_CBox(glyph->glyph, FT_GLYPH_BBOX_SUBPIXELS, &glyph->bbox);

        /* cache the newly created glyph in hash table */
        glyph_hash_insert(&s->glyph_hash, glyph);
    } else {
        if (s->borderw && !glyph->border_glyph) {
            glyph->border_glyph = glyph->glyph;
            if (FT_Glyph_StrokeBorder(&glyph->border_glyph, s->stroker, 0, 0)) {
                ret = AVERROR_EXTERNAL;
                goto error;
            }
        }
    }

    // Check if a bitmap is needed
    if (shift_x64 >= 0 && shift_y64 >= 0) {
        // Get the bitmap subpixel index (0 -> 15)
        int idx = get_subpixel_idx(shift_x64, shift_y64);
        shift.x = shift_x64;
        shift.y = shift_y64;

        if (!glyph->bglyph[idx]) {
            /* Try disk cache first */
            FT_BitmapGlyph cached = disk_cache_try_load(ctx, s, code, s->fontsize, idx, 0);
            if (cached) {
                glyph->bglyph[idx] = cached;
                glyph->bglyph_from_cache |= (1 << idx);
            } else {
                FT_Glyph tmp_glyph = glyph->glyph;
                av_log(ctx, AV_LOG_DEBUG, "Glyph cache: rendering code=0x%x size=%u subpx=%d border=0 via FreeType\n",
                       code, s->fontsize, idx);
                if (FT_Glyph_To_Bitmap(&tmp_glyph, FT_RENDER_MODE_NORMAL, &shift, 0)) {
                    ret = AVERROR_EXTERNAL;
                    goto error;
                }
                glyph->bglyph[idx] = (FT_BitmapGlyph)tmp_glyph;
                if (glyph->bglyph[idx]->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                    av_log(ctx, AV_LOG_ERROR, "Monocromatic (1bpp) fonts are not supported.\n");
                    ret = AVERROR(EINVAL);
                    goto error;
                }
                /* Save to disk cache */
                disk_cache_save(ctx, s, code, s->fontsize, idx, 0, glyph->bglyph[idx]);
            }
        }
        if (s->borderw && !glyph->border_bglyph[idx]) {
            /* Try disk cache first */
            FT_BitmapGlyph cached = disk_cache_try_load(ctx, s, code, s->fontsize, idx, 1);
            if (cached) {
                glyph->border_bglyph[idx] = cached;
                glyph->border_bglyph_from_cache |= (1 << idx);
            } else {
                FT_Glyph tmp_glyph = glyph->border_glyph;
                av_log(ctx, AV_LOG_DEBUG, "Glyph cache: rendering code=0x%x size=%u subpx=%d border=1 via FreeType\n",
                       code, s->fontsize, idx);
                if (FT_Glyph_To_Bitmap(&tmp_glyph, FT_RENDER_MODE_NORMAL, &shift, 0)) {
                    ret = AVERROR_EXTERNAL;
                    goto error;
                }
                glyph->border_bglyph[idx] = (FT_BitmapGlyph)tmp_glyph;
                /* Save to disk cache */
                disk_cache_save(ctx, s, code, s->fontsize, idx, 1, glyph->border_bglyph[idx]);
            }
        }
    }
    if (glyph_ptr) {
        *glyph_ptr = glyph;
    }
    return 0;

error:
    if (glyph && glyph->glyph)
        FT_Done_Glyph(glyph->glyph);

    av_freep(&glyph);
    return ret;
}

// Convert a string formatted as "n1|n2|...|nN" into an integer array
static int string_to_array(const char *source, int *result, int result_size)
{
    int counter = 0, size = strlen(source) + 1;
    char *saveptr, *curval, *dup = av_malloc(size);
    if (!dup)
        return 0;
    av_strlcpy(dup, source, size);
    if (result_size > 0 && (curval = av_strtok(dup, "|", &saveptr))) {
        do {
            result[counter++] = atoi(curval);
        } while ((curval = av_strtok(NULL, "|", &saveptr)) && counter < result_size);
    }
    av_free(dup);
    return counter;
}

static int func_pict_type(void *ctx, AVBPrint *bp, const char *function_name, unsigned argc, char **argv)
{
    DrawTextContext *s = ((AVFilterContext *)ctx)->priv;

    av_bprintf(bp, "%c", av_get_picture_type_char(s->var_values[VAR_PICT_TYPE]));
    return 0;
}

static int func_pts(void *ctx, AVBPrint *bp, const char *function_name, unsigned argc, char **argv)
{
    DrawTextContext *s = ((AVFilterContext *)ctx)->priv;
    const char *fmt;
    const char *strftime_fmt = NULL;
    const char *delta = NULL;
    double pts = s->var_values[VAR_T];

    // argv: pts, FMT, [DELTA, 24HH | strftime_fmt]

    fmt = argc >= 1 ? argv[0] : "flt";
    if (argc >= 2) {
        delta = argv[1];
    }
    if (argc >= 3) {
        if (!strcmp(fmt, "hms")) {
            if (!strcmp(argv[2], "24HH")) {
                av_log(ctx, AV_LOG_WARNING, "pts third argument 24HH is deprecated, use pts:hms24hh instead\n");
                fmt = "hms24";
            } else {
                av_log(ctx, AV_LOG_ERROR, "Invalid argument '%s', '24HH' was expected\n", argv[2]);
                return AVERROR(EINVAL);
            }
        } else {
            strftime_fmt = argv[2];
        }
    }

    return ff_print_pts(ctx, bp, pts, delta, fmt, strftime_fmt);
}

static int func_frame_num(void *ctx, AVBPrint *bp, const char *function_name, unsigned argc, char **argv)
{
    DrawTextContext *s = ((AVFilterContext *)ctx)->priv;

    av_bprintf(bp, "%d", (int)s->var_values[VAR_N]);
    return 0;
}

static int func_metadata(void *ctx, AVBPrint *bp, const char *function_name, unsigned argc, char **argv)
{
    DrawTextContext *s = ((AVFilterContext *)ctx)->priv;
    AVDictionaryEntry *e = av_dict_get(s->metadata, argv[0], NULL, 0);

    if (e && e->value)
        av_bprintf(bp, "%s", e->value);
    else if (argc >= 2)
        av_bprintf(bp, "%s", argv[1]);
    return 0;
}

static int func_strftime(void *ctx, AVBPrint *bp, const char *function_name, unsigned argc, char **argv)
{
    const char *strftime_fmt = argc ? argv[0] : NULL;

    return ff_print_time(ctx, bp, strftime_fmt, !strcmp(function_name, "localtime"));
}

static int func_eval_expr(void *ctx, AVBPrint *bp, const char *function_name, unsigned argc, char **argv)
{
    DrawTextContext *s = ((AVFilterContext *)ctx)->priv;

    return ff_print_eval_expr(ctx, bp, argv[0],
                              fun2_names, fun2,
                              var_names, s->var_values, &s->prng);
}

static int func_eval_expr_int_format(void *ctx, AVBPrint *bp, const char *function_name, unsigned argc, char **argv)
{
    DrawTextContext *s = ((AVFilterContext *)ctx)->priv;
    int ret;
    int positions = -1;

    /*
     * argv[0] expression to be converted to `int`
     * argv[1] format: 'x', 'X', 'd' or 'u'
     * argv[2] positions printed (optional)
     */

    if (argc == 3) {
        ret = sscanf(argv[2], "%u", &positions);
        if (ret != 1) {
            av_log(ctx, AV_LOG_ERROR, "expr_int_format(): Invalid number of positions"
                    " to print: '%s'\n", argv[2]);
            return AVERROR(EINVAL);
        }
    }

    return ff_print_formatted_eval_expr(ctx, bp, argv[0],
                                        fun2_names, fun2,
                                        var_names, s->var_values,
                                        &s->prng,
                                        argv[1][0], positions);
}

static const FFExpandTextFunction expand_text_functions[] = {
    { "e",               1, 1, func_eval_expr },
    { "eif",             2, 3, func_eval_expr_int_format },
    { "expr",            1, 1, func_eval_expr },
    { "expr_int_format", 2, 3, func_eval_expr_int_format },
    { "frame_num",       0, 0, func_frame_num },
    { "gmtime",          0, 1, func_strftime },
    { "localtime",       0, 1, func_strftime },
    { "metadata",        1, 2, func_metadata },
    { "n",               0, 0, func_frame_num },
    { "pict_type",       0, 0, func_pict_type },
    { "pts",             0, 3, func_pts }
};

static av_cold int init(AVFilterContext *ctx)
{
    int err;
    DrawTextContext *s = ctx->priv;

    av_expr_free(s->fontsize_pexpr);
    s->fontsize_pexpr = NULL;

    s->fontsize = 0;
    s->default_fontsize = 16;
    s->prev_first_visible_line = -1;
    s->prev_last_visible_line  = -1;

    if (!s->fontfile && !CONFIG_LIBFONTCONFIG) {
        av_log(ctx, AV_LOG_ERROR, "No font filename provided\n");
        return AVERROR(EINVAL);
    }

    if (s->textfile) {
        if (s->text) {
            av_log(ctx, AV_LOG_ERROR,
                   "Both text and text file provided. Please provide only one\n");
            return AVERROR(EINVAL);
        }
        if ((err = ff_load_textfile(ctx, (const char *)s->textfile, &s->text, NULL)) < 0)
            return err;
        s->text_version = 1;
    }

    if (s->reload && !s->textfile)
        av_log(ctx, AV_LOG_WARNING, "No file to reload\n");

    if (s->tc_opt_string) {
        int ret = av_timecode_init_from_string(&s->tc, s->tc_rate,
                                               s->tc_opt_string, ctx);
        if (ret < 0)
            return ret;
        if (s->tc24hmax)
            s->tc.flags |= AV_TIMECODE_FLAG_24HOURSMAX;
        if (!s->text)
            s->text = av_strdup("");
    }

    if (s->text_source_string) {
        s->text_source = text_source_string_parse(s->text_source_string);
        if ((int)s->text_source < 0) {
            av_log(ctx, AV_LOG_ERROR, "Error text source: %s\n", s->text_source_string);
            return AVERROR(EINVAL);
        }
    }

    if (s->text_source == AV_FRAME_DATA_DETECTION_BBOXES) {
        if (s->text) {
            av_log(ctx, AV_LOG_WARNING, "Multiple texts provided, will use text_source only\n");
            av_free(s->text);
        }
        s->text = av_mallocz((AV_DETECTION_BBOX_LABEL_NAME_MAX_SIZE + 1) *
                             (AV_NUM_DETECTION_BBOX_CLASSIFY + 1));
        if (!s->text)
            return AVERROR(ENOMEM);
    }

    if (!s->text) {
        av_log(ctx, AV_LOG_ERROR,
               "Either text, a valid file, a timecode or text source must be provided\n");
        return AVERROR(EINVAL);
    }

    s->expand_text = (FFExpandTextContext) {
        .log_ctx = ctx,
        .functions = expand_text_functions,
        .functions_nb = FF_ARRAY_ELEMS(expand_text_functions)
    };

#if CONFIG_LIBFRIBIDI
    if (s->text_shaping)
        if ((err = shape_text(ctx)) < 0)
            return err;
#endif

    if ((err = FT_Init_FreeType(&(s->library)))) {
        av_log(ctx, AV_LOG_ERROR,
               "Could not load FreeType: %s\n", FT_ERRMSG(err));
        return AVERROR(EINVAL);
    }

    if ((err = load_font(ctx)) < 0)
        return err;

    /* Initialize disk-based glyph cache if enabled */
    if (!s->glyph_cache_dir) {
        av_log(ctx, AV_LOG_DEBUG, "Glyph disk cache disabled: glyph_cache option not set\n");
    } else if (!s->font_path) {
        av_log(ctx, AV_LOG_DEBUG, "Glyph disk cache disabled: no font path resolved\n");
    } else {
        struct stat st;
        s->font_hash = compute_file_hash(s->font_path);
        if (s->font_hash == 0) {
            av_log(ctx, AV_LOG_WARNING, "Glyph disk cache disabled: could not hash font file '%s'\n",
                   s->font_path);
        } else {
            /* Create cache directory if it doesn't exist */
            if (stat(s->glyph_cache_dir, &st) == -1) {
#ifdef _WIN32
                if (mkdir(s->glyph_cache_dir) == 0) {
#else
                if (mkdir(s->glyph_cache_dir, 0755) == 0) {
#endif
                    s->glyph_cache_initialized = 1;
                    av_log(ctx, AV_LOG_INFO, "Glyph disk cache enabled: %s (font: %s, hash: %016" PRIx64 ") [created]\n",
                           s->glyph_cache_dir, s->font_path, s->font_hash);
                } else {
                    av_log(ctx, AV_LOG_WARNING, "Glyph disk cache disabled: could not create directory '%s'\n",
                           s->glyph_cache_dir);
                }
            } else if (S_ISDIR(st.st_mode)) {
                s->glyph_cache_initialized = 1;
                av_log(ctx, AV_LOG_INFO, "Glyph disk cache enabled: %s (font: %s, hash: %016" PRIx64 ")\n",
                       s->glyph_cache_dir, s->font_path, s->font_hash);
            } else {
                av_log(ctx, AV_LOG_WARNING, "Glyph disk cache disabled: '%s' exists but is not a directory\n",
                       s->glyph_cache_dir);
            }
        }
    }

    if ((err = update_fontsize(ctx)) < 0)
        return err;

    /* Create HarfBuzz font using memory-mapped blob (and optionally subset font) */
    if ((err = create_hb_font_mmap(ctx, s)) < 0)
        return err;

    /* Warm up HarfBuzz to parse OpenType tables upfront */
    harfbuzz_warmup(s->hb_font);

    // Always init the stroker, may be needed if borderw is set via command
    if (FT_Stroker_New(s->library, &s->stroker)) {
        av_log(ctx, AV_LOG_ERROR, "Could not init FT stroker\n");
        return AVERROR_EXTERNAL;
    }

    if (s->borderw) {
        FT_Stroker_Set(s->stroker, s->borderw << 6, FT_STROKER_LINECAP_ROUND,
                       FT_STROKER_LINEJOIN_ROUND, 0);
    }

    /* load the fallback glyph with code 0 */
    load_glyph(ctx, NULL, 0, 0, 0);

    if (s->exp_mode == EXP_STRFTIME &&
        (strchr(s->text, '%') || strchr(s->text, '\\')))
        av_log(ctx, AV_LOG_WARNING, "expansion=strftime is deprecated.\n");

    av_bprint_init(&s->expanded_text, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprint_init(&s->expanded_fontcolor, 0, AV_BPRINT_SIZE_UNLIMITED);

    return 0;
}

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    return ff_set_common_formats2(ctx, cfg_in, cfg_out,
                                  ff_draw_supported_pixel_formats(0));
}

static int glyph_enu_border_free(void *opaque, void *elem)
{
    Glyph *glyph = elem;

    if (glyph->border_glyph != NULL) {
        for (int t = 0; t < 16; ++t) {
            if (glyph->border_bglyph[t] != NULL) {
                free_bitmap_glyph(glyph->border_bglyph[t], (glyph->border_bglyph_from_cache >> t) & 1);
                glyph->border_bglyph[t] = NULL;
            }
        }
        glyph->border_bglyph_from_cache = 0;
        FT_Done_Glyph(glyph->border_glyph);
        glyph->border_glyph = NULL;
    }
    return 0;
}

static int glyph_enu_free(void *opaque, void *elem)
{
    Glyph *glyph = elem;

    FT_Done_Glyph(glyph->glyph);
    FT_Done_Glyph(glyph->border_glyph);
    for (int t = 0; t < 16; ++t) {
        free_bitmap_glyph(glyph->bglyph[t], (glyph->bglyph_from_cache >> t) & 1);
        free_bitmap_glyph(glyph->border_bglyph[t], (glyph->border_bglyph_from_cache >> t) & 1);
    }
    av_free(elem);
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    DrawTextContext *s = ctx->priv;

    av_expr_free(s->x_pexpr);
    av_expr_free(s->y_pexpr);
    av_expr_free(s->a_pexpr);
    av_expr_free(s->fontsize_pexpr);
    av_expr_free(s->canvas_w_pexpr);
    av_expr_free(s->canvas_h_pexpr);
    av_expr_free(s->canvas_x_pexpr);
    av_expr_free(s->canvas_y_pexpr);

    s->x_pexpr = s->y_pexpr = s->a_pexpr = s->fontsize_pexpr = NULL;
    s->canvas_w_pexpr = s->canvas_h_pexpr = s->canvas_x_pexpr = s->canvas_y_pexpr = NULL;

    glyph_hash_free(&s->glyph_hash);
    word_mem_cache_free(&s->word_cache);

    if (s->lines) {
        for (int l = 0; l < s->line_count; l++) {
            av_freep(&s->lines[l].glyphs);
            hb_destroy(&s->lines[l].hb_data);
        }
        av_freep(&s->lines);
    }
    av_freep(&s->tab_clusters);

    hb_font_destroy(s->hb_font);
    s->hb_font = NULL;
    if (s->hb_face) {
        hb_face_destroy(s->hb_face);
        s->hb_face = NULL;
    }
    if (s->hb_blob) {
        hb_blob_destroy(s->hb_blob);
        s->hb_blob = NULL;
    }
    FT_Done_Face(s->face);
    FT_Stroker_Done(s->stroker);
    FT_Done_FreeType(s->library);

    av_freep(&s->font_path);

    av_bprint_finalize(&s->expanded_text, NULL);
    av_bprint_finalize(&s->expanded_fontcolor, NULL);
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    DrawTextContext *s = ctx->priv;
    char *expr;
    int ret;

    ret = ff_draw_init_from_link(&s->dc, inlink, FF_DRAW_PROCESS_ALPHA);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to initialize FFDrawContext\n");
        return ret;
    }
    ff_draw_color(&s->dc, &s->fontcolor,   s->fontcolor.rgba);
    ff_draw_color(&s->dc, &s->shadowcolor, s->shadowcolor.rgba);
    ff_draw_color(&s->dc, &s->bordercolor, s->bordercolor.rgba);
    ff_draw_color(&s->dc, &s->boxcolor,    s->boxcolor.rgba);

    s->var_values[VAR_w]    = s->var_values[VAR_W] = s->var_values[VAR_MAIN_W] = inlink->w;
    s->var_values[VAR_h]    = s->var_values[VAR_H] = s->var_values[VAR_MAIN_H] = inlink->h;
    s->var_values[VAR_SAR]  = inlink->sample_aspect_ratio.num ? av_q2d(inlink->sample_aspect_ratio) : 1;
    s->var_values[VAR_DAR]  = (double)inlink->w / inlink->h * s->var_values[VAR_SAR];
    s->var_values[VAR_HSUB] = 1 << s->dc.hsub_max;
    s->var_values[VAR_VSUB] = 1 << s->dc.vsub_max;
    s->var_values[VAR_X]    = NAN;
    s->var_values[VAR_Y]    = NAN;
    s->var_values[VAR_T]    = NAN;

    av_lfg_init(&s->prng, av_get_random_seed());

    av_expr_free(s->x_pexpr);
    av_expr_free(s->y_pexpr);
    av_expr_free(s->a_pexpr);
    av_expr_free(s->canvas_w_pexpr);
    av_expr_free(s->canvas_h_pexpr);
    av_expr_free(s->canvas_x_pexpr);
    av_expr_free(s->canvas_y_pexpr);
    s->x_pexpr = s->y_pexpr = s->a_pexpr = NULL;
    s->canvas_w_pexpr = s->canvas_h_pexpr = s->canvas_x_pexpr = s->canvas_y_pexpr = NULL;

    if ((ret = av_expr_parse(&s->x_pexpr, expr = s->x_expr, var_names,
                             NULL, NULL, fun2_names, fun2, 0, ctx)) < 0 ||
        (ret = av_expr_parse(&s->y_pexpr, expr = s->y_expr, var_names,
                             NULL, NULL, fun2_names, fun2, 0, ctx)) < 0 ||
        (ret = av_expr_parse(&s->a_pexpr, expr = s->a_expr, var_names,
                             NULL, NULL, fun2_names, fun2, 0, ctx)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to parse expression: %s \n", expr);
        return AVERROR(EINVAL);
    }

    if ((ret = av_expr_parse(&s->canvas_w_pexpr, expr = s->canvas_w_expr, var_names,
                             NULL, NULL, fun2_names, fun2, 0, ctx)) < 0 ||
        (ret = av_expr_parse(&s->canvas_h_pexpr, expr = s->canvas_h_expr, var_names,
                             NULL, NULL, fun2_names, fun2, 0, ctx)) < 0 ||
        (ret = av_expr_parse(&s->canvas_x_pexpr, expr = s->canvas_x_expr, var_names,
                             NULL, NULL, fun2_names, fun2, 0, ctx)) < 0 ||
        (ret = av_expr_parse(&s->canvas_y_pexpr, expr = s->canvas_y_expr, var_names,
                             NULL, NULL, fun2_names, fun2, 0, ctx)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to parse canvas expression: %s \n", expr);
        return AVERROR(EINVAL);
    }

    return 0;
}

static int command(AVFilterContext *ctx, const char *cmd, const char *arg, char *res, int res_len, int flags)
{
    DrawTextContext *old = ctx->priv;
    DrawTextContext *new = NULL;
    int ret;

    if (!strcmp(cmd, "reinit")) {
        new = av_mallocz(sizeof(DrawTextContext));
        if (!new)
            return AVERROR(ENOMEM);

        new->class = &drawtext_class;
        ret = av_opt_copy(new, old);
        if (ret < 0)
            goto fail;

        ctx->priv = new;
        ret = av_set_options_string(ctx, arg, "=", ":");
        if (ret < 0) {
            ctx->priv = old;
            goto fail;
        }

        ret = init(ctx);
        if (ret < 0) {
            uninit(ctx);
            ctx->priv = old;
            goto fail;
        }

        new->reinit = 1;

        ctx->priv = old;
        uninit(ctx);
        av_opt_free(old);
        av_freep(&old);

        ctx->priv = new;
        return config_input(ctx->inputs[0]);
    } else {
        int old_borderw = old->borderw;
        if ((ret = ff_filter_process_command(ctx, cmd, arg, res, res_len, flags)) < 0) {
            return ret;
        }
        if (old->borderw != old_borderw) {
            FT_Stroker_Set(old->stroker, old->borderw << 6, FT_STROKER_LINECAP_ROUND,
                        FT_STROKER_LINEJOIN_ROUND, 0);
            // Dispose the old border glyphs
            glyph_hash_free_borders(&old->glyph_hash);
        } else if (strcmp(cmd, "fontsize") == 0) {
            av_expr_free(old->fontsize_pexpr);
            old->fontsize_pexpr = NULL;
            old->blank_advance64 = 0;
        }
        return config_input(ctx->inputs[0]);
    }

fail:
    av_log(ctx, AV_LOG_ERROR, "Failed to process command. Continuing with existing parameters.\n");
    av_freep(&new);
    return ret;
}

static void update_color_with_alpha(DrawTextContext *s, FFDrawColor *color, const FFDrawColor incolor)
{
    *color = incolor;
    color->rgba[3] = (color->rgba[3] * s->alpha) / 255;
    ff_draw_color(&s->dc, color, color->rgba);
}

static void update_alpha(DrawTextContext *s)
{
    double alpha = av_expr_eval(s->a_pexpr, s->var_values, &s->prng);

    if (isnan(alpha))
        return;

    if (alpha >= 1.0)
        s->alpha = 255;
    else if (alpha <= 0)
        s->alpha = 0;
    else
        s->alpha = 256 * alpha;
}

static int draw_glyphs(AVFilterContext *ctx, AVFrame *frame,
                       FFDrawColor *color,
                       TextMetrics *metrics,
                       int x, int y, int borderw,
                       int canvas_x, int canvas_y,
                       int canvas_w, int canvas_h,
                       int text_w, int text_h, int canvas_tile)
{
    DrawTextContext *s = ctx->priv;
    int g, l, x1, y1, w1, h1, idx;
    int dx = 0, dy = 0, pdx = 0;
    GlyphInfo *info;
    Glyph dummy = { 0 }, *glyph;
    FT_Bitmap bitmap;
    FT_BitmapGlyph b_glyph;
    uint8_t j_left = 0, j_right = 0, j_top = 0, j_bottom = 0;
    int line_w, offset_y = 0;
    int clip_x = 0, clip_y = 0;
    int use_canvas = canvas_w > 0 && canvas_h > 0;
    int tile_x, tile_y;  // for looping

    j_left = !!(s->text_align & TA_LEFT);
    j_right = !!(s->text_align & TA_RIGHT);
    j_top = !!(s->text_align & TA_TOP);
    j_bottom = !!(s->text_align & TA_BOTTOM);

    if (j_top && j_bottom) {
        offset_y = (s->box_height - metrics->height) / 2;
    } else if (j_bottom) {
        offset_y = s->box_height - metrics->height;
    }

    if ((!j_left || j_right) && !s->tab_warning_printed && s->tab_count > 0) {
        s->tab_warning_printed = 1;
        av_log(ctx, AV_LOG_WARNING, "Tab characters are only supported with left horizontal alignment\n");
    }

    if (use_canvas) {
        clip_x = FFMIN(metrics->rect_x + canvas_w + s->bb_right, frame->width);
        clip_y = FFMIN(metrics->rect_y + canvas_h + s->bb_bottom, frame->height);
    } else {
        clip_x = FFMIN(metrics->rect_x + s->box_width + s->bb_right, frame->width);
        clip_y = FFMIN(metrics->rect_y + s->box_height + s->bb_bottom, frame->height);
    }

    // Determine tile range for looping
    int tile_x_start = 0, tile_x_end = 0;
    int tile_y_start = 0, tile_y_end = 0;
    int tile_w = text_w + 1;  // add 1 pixel spacing between tiles
    int tile_h = text_h + 1;
    if (use_canvas && canvas_tile && text_w > 0 && text_h > 0) {
        // Calculate which tiles we need to render to cover the canvas
        // tile offset of -1 means render text shifted left by tile_w, etc.
        tile_x_start = (canvas_x / tile_w) - 1;
        tile_x_end = ((canvas_x + canvas_w) / tile_w) + 1;
        tile_y_start = (canvas_y / tile_h) - 1;
        tile_y_end = ((canvas_y + canvas_h) / tile_h) + 1;
    }

    // In non-tile mode restrict to the visible line window so we don't
    // traverse all N line structs just to skip them.
    int l_start = 0, l_end = s->line_count - 1;
    if (!canvas_tile && s->prev_first_visible_line >= 0) {
        l_start = s->prev_first_visible_line;
        l_end   = s->prev_last_visible_line;
    }

    for (tile_y = tile_y_start; tile_y <= tile_y_end; ++tile_y) {
    for (tile_x = tile_x_start; tile_x <= tile_x_end; ++tile_x) {
    for (l = l_start; l <= l_end; ++l) {
        TextLine *line = &s->lines[l];

        /* Skip lines that weren't shaped (viewport optimization) */
        if (!line->is_shaped || line->glyphs == NULL) {
            continue;
        }

        line_w = line->width_px;

        /* Word-level caching: iterate over words in the line */
        if (line->text_start && line->text_len > 0 && s->glyph_cache_dir && s->glyph_cache_initialized) {
            const char *text = line->text_start;
            int text_len = line->text_len;
            int word_start_char = 0;  /* character index of word start */
            int word_start_glyph = 0; /* glyph index of word start */
            int glyph_idx = 0;
            int base_x_offset = 0;    /* alignment offset */

            if (j_left && j_right) {
                base_x_offset = (s->box_width - line_w) / 2;
            } else if (j_right) {
                base_x_offset = s->box_width - line_w;
            }

            /* Find words by scanning for spaces */
            for (int char_idx = 0; char_idx <= text_len; char_idx++) {
                int is_end = (char_idx == text_len);
                int is_space = !is_end && (text[char_idx] == ' ' || text[char_idx] == '\t');

                if (is_space || is_end) {
                    int word_len = char_idx - word_start_char;

                    if (word_len > 0) {
                        /* Find glyph range for this word using cluster indices */
                        int word_end_glyph = word_start_glyph;
                        while (word_end_glyph < line->hb_data.glyph_count) {
                            hb_glyph_info_t *hb_info = &line->hb_data.glyph_info[word_end_glyph];
                            if ((int)hb_info->cluster >= line->cluster_offset + char_idx)
                                break;
                            word_end_glyph++;
                        }

                        int glyph_count = word_end_glyph - word_start_glyph;
                        if (glyph_count > 0) {
                            WordBitmap word_cache = {0};
                            int word_x_offset = (word_start_glyph > 0) ? line->glyphs[word_start_glyph].x : 0;
                            int cache_hit = (word_cache_try_load(ctx, s, text + word_start_char, word_len,
                                                                 borderw, &word_cache) == 0);

                            if (cache_hit && word_cache.buffer) {
                                /* Cache hit - blit cached word */
                                int wx = x + word_x_offset + word_cache.left + base_x_offset;
                                int wy = y - word_cache.top + offset_y;

                                if (use_canvas) {
                                    wx -= canvas_x;
                                    wy -= canvas_y;
                                    if (canvas_tile) {
                                        wx += tile_x * tile_w;
                                        wy += tile_y * tile_h;
                                    }
                                }

                                int wdx = 0, wdy = 0;
                                int ww = word_cache.width, wh = word_cache.height;
                                if (wx < metrics->rect_x - s->bb_left) {
                                    wdx = metrics->rect_x - s->bb_left - wx;
                                    wx = metrics->rect_x - s->bb_left;
                                }
                                if (wy < metrics->rect_y - s->bb_top) {
                                    wdy = metrics->rect_y - s->bb_top - wy;
                                    wy = metrics->rect_y - s->bb_top;
                                }
                                if (wdx < ww && wdy < wh && wx < clip_x && wy < clip_y) {
                                    ww = FFMIN(clip_x - wx, ww - wdx);
                                    wh = FFMIN(clip_y - wy, wh - wdy);
                                    ff_blend_mask(&s->dc, color, frame->data, frame->linesize, clip_x, clip_y,
                                                  word_cache.buffer + wdy * word_cache.pitch + wdx,
                                                  word_cache.pitch, ww, wh, 3, 0, wx, wy);
                                }
                                word_bitmap_free(&word_cache);
                            } else {
                                /* Cache miss - render glyphs and save to cache */
                                for (int wg = word_start_glyph; wg < word_end_glyph; wg++) {
                                    GlyphInfo *winfo = &line->glyphs[wg];
                                    Glyph *wglyph = glyph_hash_find(&s->glyph_hash, winfo->code, s->fontsize);
                                    if (!wglyph) continue;

                                    int widx = get_subpixel_idx(winfo->shift_x64, winfo->shift_y64);
                                    FT_BitmapGlyph wb_glyph = borderw ? wglyph->border_bglyph[widx] : wglyph->bglyph[widx];
                                    if (!wb_glyph) continue;

                                    FT_Bitmap wbitmap = wb_glyph->bitmap;
                                    int wx1 = x + winfo->x + wb_glyph->left + base_x_offset;
                                    int wy1 = y + winfo->y - wb_glyph->top + offset_y;

                                    if (use_canvas) {
                                        wx1 -= canvas_x;
                                        wy1 -= canvas_y;
                                        if (canvas_tile) {
                                            wx1 += tile_x * tile_w;
                                            wy1 += tile_y * tile_h;
                                        }
                                    }

                                    int ww1 = wbitmap.width, wh1 = wbitmap.rows;
                                    int wdx1 = 0, wdy1 = 0;
                                    if (wx1 < metrics->rect_x - s->bb_left) {
                                        wdx1 = metrics->rect_x - s->bb_left - wx1;
                                        wx1 = metrics->rect_x - s->bb_left;
                                    }
                                    if (wy1 < metrics->rect_y - s->bb_top) {
                                        wdy1 = metrics->rect_y - s->bb_top - wy1;
                                        wy1 = metrics->rect_y - s->bb_top;
                                    }
                                    if (wdx1 < ww1 && wdy1 < wh1 && wx1 < clip_x && wy1 < clip_y) {
                                        int wpdx = wdx1 + wdy1 * wbitmap.pitch;
                                        ww1 = FFMIN(clip_x - wx1, ww1 - wdx1);
                                        wh1 = FFMIN(clip_y - wy1, wh1 - wdy1);
                                        ff_blend_mask(&s->dc, color, frame->data, frame->linesize, clip_x, clip_y,
                                                      wbitmap.buffer + wpdx, wbitmap.pitch, ww1, wh1, 3, 0, wx1, wy1);
                                    }
                                }

                                /* Save word to cache */
                                WordBitmap save_bmp = {0};
                                if (render_glyphs_to_bitmap(s, line->glyphs, word_start_glyph, glyph_count,
                                                           word_x_offset, borderw, &save_bmp) == 0 && save_bmp.buffer) {
                                    word_cache_save(ctx, s, text + word_start_char, word_len, borderw, &save_bmp);
                                    word_bitmap_free(&save_bmp);
                                }
                            }
                        }
                        word_start_glyph = word_end_glyph;
                    }
                    word_start_char = char_idx + 1;
                    /* Skip past space glyphs */
                    while (word_start_glyph < line->hb_data.glyph_count) {
                        hb_glyph_info_t *hb_info = &line->hb_data.glyph_info[word_start_glyph];
                        if ((int)hb_info->cluster >= line->cluster_offset + word_start_char)
                            break;
                        word_start_glyph++;
                    }
                }
            }
            continue;  /* Skip legacy per-glyph loop */
        }

        /* Fallback: per-glyph rendering when caching is disabled */
        for (g = 0; g < line->hb_data.glyph_count; ++g) {
            info = &line->glyphs[g];
            glyph = glyph_hash_find(&s->glyph_hash, info->code, s->fontsize);
            if (!glyph) {
                return AVERROR(EINVAL);
            }

            idx = get_subpixel_idx(info->shift_x64, info->shift_y64);
            b_glyph = borderw ? glyph->border_bglyph[idx] : glyph->bglyph[idx];
            bitmap = b_glyph->bitmap;
            x1 = x + info->x + b_glyph->left;
            y1 = y + info->y - b_glyph->top + offset_y;

            // Apply canvas viewport offset and tiling
            if (use_canvas) {
                x1 -= canvas_x;
                y1 -= canvas_y;
                if (canvas_tile) {
                    x1 += tile_x * tile_w;
                    y1 += tile_y * tile_h;
                }
            }

            w1 = bitmap.width;
            h1 = bitmap.rows;

            if (j_left && j_right) {
                x1 += (s->box_width - line_w) / 2;
            } else if (j_right) {
                x1 += s->box_width - line_w;
            }

            // Offset of the glyph's bitmap in the visible region
            dx = dy = 0;
            if (x1 < metrics->rect_x - s->bb_left) {
                dx = metrics->rect_x - s->bb_left - x1;
                x1 = metrics->rect_x - s->bb_left;
            }
            if (y1 < metrics->rect_y - s->bb_top) {
                dy = metrics->rect_y - s->bb_top - y1;
                y1 = metrics->rect_y - s->bb_top;
            }

            // check if the glyph is empty or out of the clipping region
            if (dx >= w1 || dy >= h1 || x1 >= clip_x || y1 >= clip_y) {
                continue;
            }

            pdx = dx + dy * bitmap.pitch;
            w1 = FFMIN(clip_x - x1, w1 - dx);
            h1 = FFMIN(clip_y - y1, h1 - dy);

            ff_blend_mask(&s->dc, color, frame->data, frame->linesize, clip_x, clip_y,
                bitmap.buffer + pdx, bitmap.pitch, w1, h1, 3, 0, x1, y1);
        }
    }
    }
    }

    return 0;
}

/**
 * Warm up HarfBuzz by shaping a representative string.
 * This triggers parsing of OpenType tables (GSUB/GPOS) upfront,
 * avoiding cold-start latency on the first real hb_shape() call.
 */
static void harfbuzz_warmup(hb_font_t *hb_font)
{
    hb_buffer_t *buf;
    /* Representative ASCII string covering uppercase, lowercase, and digits
     * to trigger common OpenType feature lookups (kerning, ligatures, etc.) */
    static const char warmup_text[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

    buf = hb_buffer_create();
    if (!hb_buffer_allocation_successful(buf)) {
        hb_buffer_destroy(buf);
        return;
    }

    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
    hb_buffer_set_language(buf, hb_language_from_string("en", -1));
    hb_buffer_add_utf8(buf, warmup_text, sizeof(warmup_text) - 1, 0, -1);

    /* This call parses GSUB/GPOS tables and caches them internally */
    hb_shape(hb_font, buf, NULL, 0);

    hb_buffer_destroy(buf);
}

/**
 * Populate HarfbuzzData from a cached ShapeCacheEntry.
 * Allocates owned arrays that will be freed by hb_destroy().
 */
static int hb_from_cache(HarfbuzzData *hb, const ShapeCacheEntry *entry)
{
    hb_glyph_info_t *info;
    hb_glyph_position_t *pos;

    /* Free any existing owned arrays */
    if (hb->owns_arrays) {
        av_freep(&hb->glyph_info);
        av_freep(&hb->glyph_pos);
    }

    hb->glyph_count = entry->glyph_count;
    if (entry->glyph_count == 0) {
        hb->glyph_info = NULL;
        hb->glyph_pos = NULL;
        hb->owns_arrays = 0;
        return 0;
    }

    info = av_malloc_array(entry->glyph_count, sizeof(*info));
    pos = av_malloc_array(entry->glyph_count, sizeof(*pos));
    if (!info || !pos) {
        av_free(info);
        av_free(pos);
        return AVERROR(ENOMEM);
    }

    for (unsigned int i = 0; i < entry->glyph_count; i++) {
        memset(&info[i], 0, sizeof(info[i]));
        info[i].codepoint = entry->codepoints[i];
        info[i].cluster = entry->clusters[i];

        memset(&pos[i], 0, sizeof(pos[i]));
        pos[i].x_advance = entry->x_advances[i];
        pos[i].y_advance = entry->y_advances[i];
        pos[i].x_offset = entry->x_offsets[i];
        pos[i].y_offset = entry->y_offsets[i];
    }

    hb->glyph_info = info;
    hb->glyph_pos = pos;
    hb->owns_arrays = 1;
    return 0;
}

// Shapes a line of text using libharfbuzz, with optional disk caching
static int shape_text_hb(AVFilterContext *ctx, DrawTextContext *s, HarfbuzzData* hb,
                         const char* text, int textLen)
{
    char cache_path[512];
    uint64_t text_hash = 0;
    int use_cache = s->glyph_cache_dir && s->glyph_cache_initialized;

    /* Try to load from disk cache */
    if (use_cache) {
        ShapeCacheEntry entry;
        text_hash = compute_text_hash(text, textLen);

        if (build_shape_cache_path(cache_path, sizeof(cache_path), s->glyph_cache_dir,
                                   s->font_hash, text_hash, s->fontsize) == 0) {
            if (shape_cache_read(cache_path, &entry) == 0) {
                int ret = hb_from_cache(hb, &entry);
                shape_cache_entry_free(&entry);
                if (ret == 0) {
                    av_log(ctx, AV_LOG_DEBUG, "Shape cache: hit text_hash=%016" PRIx64 " size=%u glyphs=%u\n",
                           text_hash, s->fontsize, hb->glyph_count);
                    return 0;  /* Cache hit */
                }
            }
        }
    }

    /* Cache miss or caching disabled - do normal shaping */
    if (hb->owns_arrays) {
        av_freep(&hb->glyph_info);
        av_freep(&hb->glyph_pos);
        hb->owns_arrays = 0;
    }

    if (hb->buf) {
        hb_buffer_reset(hb->buf);
    } else {
        hb->buf = hb_buffer_create();
        if (!hb_buffer_allocation_successful(hb->buf)) {
            hb->buf = NULL;
            return AVERROR(ENOMEM);
        }
    }
    hb_buffer_set_direction(hb->buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(hb->buf, HB_SCRIPT_LATIN);
    hb_buffer_set_language(hb->buf, hb_language_from_string("en", -1));
    hb_buffer_guess_segment_properties(hb->buf);
    hb_buffer_add_utf8(hb->buf, text, textLen, 0, -1);
    hb_shape(s->hb_font, hb->buf, NULL, 0);
    hb->glyph_info = hb_buffer_get_glyph_infos(hb->buf, &hb->glyph_count);
    hb->glyph_pos = hb_buffer_get_glyph_positions(hb->buf, &hb->glyph_count);

    /* Save to disk cache */
    if (use_cache && hb->glyph_count > 0) {
        if (build_shape_cache_path(cache_path, sizeof(cache_path), s->glyph_cache_dir,
                                   s->font_hash, text_hash, s->fontsize) == 0) {
            if (shape_cache_write(cache_path, hb->glyph_info, hb->glyph_pos, hb->glyph_count) == 0) {
                av_log(ctx, AV_LOG_DEBUG, "Shape cache: miss+saved text_hash=%016" PRIx64 " size=%u glyphs=%u\n",
                       text_hash, s->fontsize, hb->glyph_count);
            } else {
                av_log(ctx, AV_LOG_DEBUG, "Shape cache: miss text_hash=%016" PRIx64 " size=%u glyphs=%u (save failed)\n",
                       text_hash, s->fontsize, hb->glyph_count);
            }
        }
    }

    return 0;
}

static void hb_destroy(HarfbuzzData *hb)
{
    if (hb->buf) {
        hb_buffer_destroy(hb->buf);
        hb->buf = NULL;
    }
    if (hb->owns_arrays) {
        av_freep(&hb->glyph_info);
        av_freep(&hb->glyph_pos);
        hb->owns_arrays = 0;
    } else {
        hb->glyph_info = NULL;
        hb->glyph_pos = NULL;
    }
    hb->glyph_count = 0;
}

/**
 * Calculate which lines are visible in the canvas viewport.
 * This allows skipping expensive shaping operations for off-screen lines.
 */
static void calculate_visible_lines(int line_count, int line_height64,
                                    int line_spacing, int canvas_y, int canvas_h,
                                    int buffer_lines,
                                    int *first_line, int *last_line)
{
    int effective_line_height;

    if (canvas_h <= 0) {
        *first_line = 0;
        *last_line = line_count - 1;
        return;
    }

    effective_line_height = (line_height64 >> 6) + line_spacing;
    if (effective_line_height <= 0) effective_line_height = 1;

    *first_line = FFMAX(0, canvas_y / effective_line_height - buffer_lines);
    *last_line = FFMIN(line_count - 1,
                       (canvas_y + canvas_h) / effective_line_height + buffer_lines);
}

/**
 * Count lines and tabs in text without doing expensive shaping.
 * Stores line boundaries (text_start, text_len) in pre-allocated lines array.
 * Returns line count and tab count for preliminary height calculation.
 */
static int count_text_lines(AVFilterContext *ctx, const char *text,
                            TextLine *lines, int max_lines,
                            int *line_count_out, int *tab_count_out,
                            uint32_t *tab_clusters, int max_tabs)
{
    int line_count = 0;
    int tab_count = 0;
    int tab_start_for_line = 0;
    uint32_t code = 0;
    const uint8_t *p = (const uint8_t *)text;
    const uint8_t *line_start = p;
    int char_idx = 0;

    for (;;) {
        const uint8_t *char_start = p;
        GET_UTF8(code, *p ? *p++ : 0, code = 0xfffd; goto continue_count;);
continue_count:
        if (code == '\t') {
            if (tab_count < max_tabs) {
                tab_clusters[tab_count] = char_idx;
            }
            ++tab_count;
        }
        if (ff_is_newline(code) || code == 0) {
            if (line_count < max_lines && lines) {
                lines[line_count].text_start = (const char *)line_start;
                lines[line_count].text_len = char_start - line_start;
                lines[line_count].cluster_offset = (line_count > 0) ?
                    (lines[line_count - 1].cluster_offset + lines[line_count - 1].text_len + 1) : 0;
                lines[line_count].is_shaped = 0;
                lines[line_count].glyphs = NULL;
                lines[line_count].glyphs_alloc = 0;
                lines[line_count].first_tab_idx = tab_start_for_line;
                lines[line_count].line_tab_count = tab_count - tab_start_for_line;
                lines[line_count].metrics_fontsize = 0; // invalidate cached metrics
            }
            ++line_count;
            tab_start_for_line = tab_count;
            line_start = p;
            if (code == 0) break;
        }
        ++char_idx;
    }

    *line_count_out = line_count;
    *tab_count_out = tab_count;
    return 0;
}

/**
 * Measure and shape text lines within the visible range.
 * Lines outside the visible range are skipped (is_shaped=0).
 * Requires that s->lines, s->line_count, s->tab_clusters, s->tab_count
 * have already been set up by count_text_lines().
 */
static int measure_text(AVFilterContext *ctx, TextMetrics *metrics,
                        int first_visible_line, int last_visible_line)
{
    DrawTextContext *s = ctx->priv;
    int width64 = 0, w64 = 0;
    int cur_min_y64 = 0, first_max_y64 = -32000;
    int first_min_x64 = 32000, last_max_x64 = -32000;
    int min_y64 = 32000, max_y64 = -32000, min_x64 = 32000, max_x64 = -32000;
    Glyph *glyph = NULL;
    int ret = 0;
    char *line_buf = NULL;
    int line_buf_size = 0;

    // Evaluate the width of the space character if needed to replace tabs
    if (s->tab_count > 0 && !s->blank_advance64) {
        HarfbuzzData hb_data = {0};
        ret = shape_text_hb(ctx, s, &hb_data, " ", 1);
        if (ret != 0) {
            return ret;
        }
        s->blank_advance64 = hb_data.glyph_pos[0].x_advance;
        hb_destroy(&hb_data);
    }

    // Clear lines that were shaped last frame but are now outside the viewport.
    // Using prev_first/prev_last avoids touching all N lines every frame —
    // only the lines entering/leaving the window need work (usually O(1)/frame).
    {
        int old_first = s->prev_first_visible_line;
        int old_last  = s->prev_last_visible_line;
        if (old_first >= 0) {
            // Lines at top of old window now above the new window
            int end = FFMIN(first_visible_line, old_last + 1);
            for (int i = old_first; i < end; i++) {
                s->lines[i].is_shaped          = 0;
                s->lines[i].hb_data.glyph_count = 0;
                s->lines[i].hb_data.glyph_info  = NULL;
                s->lines[i].hb_data.glyph_pos   = NULL;
                s->lines[i].width64              = 0;
                s->lines[i].width_px             = 0;
            }
            // Lines at bottom of old window now below the new window
            int start = FFMAX(last_visible_line + 1, old_first);
            for (int i = start; i <= old_last; i++) {
                s->lines[i].is_shaped          = 0;
                s->lines[i].hb_data.glyph_count = 0;
                s->lines[i].hb_data.glyph_info  = NULL;
                s->lines[i].hb_data.glyph_pos   = NULL;
                s->lines[i].width64              = 0;
                s->lines[i].width_px             = 0;
            }
        }
    }
    s->prev_first_visible_line = first_visible_line;
    s->prev_last_visible_line  = last_visible_line;

    // Shape only visible lines — O(visible_count) per frame, not O(total_lines).
    for (int line_idx = first_visible_line; line_idx <= last_visible_line; ++line_idx) {
        TextLine *cur_line = &s->lines[line_idx];
        HarfbuzzData *hb = &cur_line->hb_data;

        // If this line was already shaped with the current fontsize, the
        // HarfBuzz glyph_info/glyph_pos and all cached metrics are still
        // valid (text is static, font unchanged).  Skip the expensive
        // reshape and the per-glyph width walk entirely.
        if (cur_line->is_shaped && cur_line->metrics_fontsize == s->fontsize) {
            w64 = cur_line->width64;
            last_max_x64 = FFMAX(cur_line->offset_right64, last_max_x64);
            cur_min_y64  = cur_line->line_min_y64;
            if (first_max_y64 == -32000)
                first_max_y64 = FFMAX(cur_line->line_max_y64, first_max_y64);
            if (first_min_x64 == 32000)
                first_min_x64 = FFMIN(cur_line->offset_left64, first_min_x64);
            min_y64 = FFMIN(cur_line->line_min_y64, min_y64);
            max_y64 = FFMAX(cur_line->line_max_y64, max_y64);
            min_x64 = FFMIN(cur_line->line_min_x64, min_x64);
            max_x64 = FFMAX(cur_line->line_max_x64, max_x64);
            if (w64 > width64)
                width64 = w64;
            continue;
        }

        // Prepare line buffer with tabs replaced by spaces
        if (cur_line->text_len + 1 > line_buf_size) {
            av_free(line_buf);
            line_buf_size = cur_line->text_len + 1;
            line_buf = av_malloc(line_buf_size);
            if (!line_buf) {
                ret = AVERROR(ENOMEM);
                goto done;
            }
        }
        memcpy(line_buf, cur_line->text_start, cur_line->text_len);
        line_buf[cur_line->text_len] = '\0';

        // Issue 6: Replace tabs with spaces in a single forward pass.
        // Previously the code restarted UTF-8 decoding from line start for each
        // tab, giving O(T * line_len) total. This advances p once per character
        // across the entire line, giving O(line_len) regardless of tab count.
        {
            const uint8_t *p = (const uint8_t *)line_buf;
            int char_idx = 0;
            int tab_end_idx = cur_line->first_tab_idx + cur_line->line_tab_count;
            for (int t = cur_line->first_tab_idx; t < tab_end_idx; ++t) {
                int tab_pos = s->tab_clusters[t] - cur_line->cluster_offset;
                if (tab_pos < 0 || tab_pos >= cur_line->text_len)
                    break;
                while (char_idx < tab_pos && *p) {
                    uint32_t code;
                    GET_UTF8(code, *p ? *p++ : 0, code = 0xfffd; p++;);
                    ++char_idx;
                }
                if (*p == '\t')
                    *(char *)p = ' ';
            }
        }

        // Shape the line (reuses existing hb_data.buf via hb_buffer_reset — Issue 5)
        ret = shape_text_hb(ctx, s, hb, line_buf, cur_line->text_len);
        if (ret != 0) {
            goto done;
        }
        cur_line->is_shaped = 1;

        // Issue 9: If per-line glyph metrics are already cached for the current
        // fontsize, skip load_glyph() and all per-glyph FFMIN/FFMAX calls.
        // Width is still recomputed from HB positions every frame (it's O(glyphs)
        // but cheap — no glyph cache lookup needed).
        if (cur_line->metrics_fontsize == s->fontsize) {
            // Cache hit — accumulate width, skip bbox work.
            int line_tab_idx = cur_line->first_tab_idx;
            int tab_end_idx  = cur_line->first_tab_idx + cur_line->line_tab_count;
            w64 = 0;
            for (unsigned int t = 0; t < hb->glyph_count; ++t) {
                uint8_t is_tab = line_tab_idx < tab_end_idx &&
                    hb->glyph_info[t].cluster ==
                        s->tab_clusters[line_tab_idx] - cur_line->cluster_offset;
                if (is_tab)
                    ++line_tab_idx;
                if (t == hb->glyph_count - 1) {
                    int last_char_width = hb->glyph_pos[t].x_advance;
                    w64 += last_char_width;
                    last_max_x64 = FFMAX(last_char_width, last_max_x64);
                    cur_line->offset_right64 = last_char_width;
                } else if (is_tab) {
                    int size = s->blank_advance64 * s->tabsize;
                    w64 = (w64 / size + 1) * size;
                } else {
                    w64 += hb->glyph_pos[t].x_advance;
                }
            }
            cur_line->width64  = w64;
            cur_line->width_px = POS_CEIL(w64, 64);

            // Update global metrics from cached per-line values.
            cur_min_y64 = cur_line->line_min_y64;
            if (line_idx == 0 || first_max_y64 == -32000)
                first_max_y64 = FFMAX(cur_line->line_max_y64, first_max_y64);
            if (line_idx == 0 || first_min_x64 == 32000)
                first_min_x64 = FFMIN(cur_line->offset_left64, first_min_x64);
            min_y64 = FFMIN(cur_line->line_min_y64, min_y64);
            max_y64 = FFMAX(cur_line->line_max_y64, max_y64);
            min_x64 = FFMIN(cur_line->line_min_x64, min_x64);
            max_x64 = FFMAX(cur_line->line_max_x64, max_x64);
        } else {
            // Cache miss — full per-glyph load+measure, then populate cache.
            int line_min_y64_new = 32000, line_max_y64_new = -32000;
            int line_min_x64_new = 32000, line_max_x64_new = -32000;
            int line_tab_idx = cur_line->first_tab_idx;
            int tab_end_idx  = cur_line->first_tab_idx + cur_line->line_tab_count;
            w64 = 0;
            cur_min_y64 = 32000;

            for (unsigned int t = 0; t < hb->glyph_count; ++t) {
                uint8_t is_tab = line_tab_idx < tab_end_idx &&
                    hb->glyph_info[t].cluster ==
                        s->tab_clusters[line_tab_idx] - cur_line->cluster_offset;
                if (is_tab)
                    ++line_tab_idx;

                ret = load_glyph(ctx, &glyph, hb->glyph_info[t].codepoint, -1, -1);
                if (ret != 0)
                    goto done;

                if (line_idx == 0 || first_max_y64 == -32000)
                    first_max_y64 = FFMAX(glyph->bbox.yMax, first_max_y64);
                if (t == 0) {
                    cur_line->offset_left64 = glyph->bbox.xMin;
                    if (line_idx == 0 || first_min_x64 == 32000)
                        first_min_x64 = FFMIN(glyph->bbox.xMin, first_min_x64);
                }
                if (t == hb->glyph_count - 1) {
                    int last_char_width = hb->glyph_pos[t].x_advance;
                    w64 += last_char_width;
                    last_max_x64 = FFMAX(last_char_width, last_max_x64);
                    cur_line->offset_right64 = last_char_width;
                } else if (is_tab) {
                    int size = s->blank_advance64 * s->tabsize;
                    w64 = (w64 / size + 1) * size;
                } else {
                    w64 += hb->glyph_pos[t].x_advance;
                }

                cur_min_y64      = FFMIN(glyph->bbox.yMin, cur_min_y64);
                line_min_y64_new = FFMIN(glyph->bbox.yMin, line_min_y64_new);
                line_max_y64_new = FFMAX(glyph->bbox.yMax, line_max_y64_new);
                line_min_x64_new = FFMIN(glyph->bbox.xMin, line_min_x64_new);
                line_max_x64_new = FFMAX(glyph->bbox.xMax, line_max_x64_new);
                min_y64          = FFMIN(glyph->bbox.yMin, min_y64);
                max_y64          = FFMAX(glyph->bbox.yMax, max_y64);
                min_x64          = FFMIN(glyph->bbox.xMin, min_x64);
                max_x64          = FFMAX(glyph->bbox.xMax, max_x64);
            }

            cur_line->width64          = w64;
            cur_line->width_px         = POS_CEIL(w64, 64);
            cur_line->line_min_y64     = line_min_y64_new;
            cur_line->line_max_y64     = line_max_y64_new;
            cur_line->line_min_x64     = line_min_x64_new;
            cur_line->line_max_x64     = line_max_x64_new;
            cur_line->metrics_fontsize = s->fontsize;
        }

        av_log(ctx, AV_LOG_DEBUG, "  Line: %d -- glyphs count: %d - width64: %d - offset_left64: %d - offset_right64: %d)\n",
            line_idx, hb->glyph_count, cur_line->width64, cur_line->offset_left64, cur_line->offset_right64);

        if (w64 > width64) {
            width64 = w64;
        }
    }

    metrics->line_height64 = s->face->size->metrics.height;

    metrics->width = POS_CEIL(width64, 64);
    if (s->y_align == YA_FONT) {
        metrics->height = POS_CEIL(metrics->line_height64 * s->line_count, 64);
    } else {
        int height64 = (metrics->line_height64 + s->line_spacing * 64) *
            (FFMAX(0, s->line_count - 1)) + first_max_y64 - cur_min_y64;
        metrics->height = POS_CEIL(height64, 64);
    }
    metrics->offset_top64 = first_max_y64;
    metrics->offset_right64 = last_max_x64;
    metrics->offset_bottom64 = cur_min_y64;
    metrics->offset_left64 = first_min_x64;
    metrics->min_x64 = min_x64;
    metrics->min_y64 = min_y64;
    metrics->max_x64 = max_x64;
    metrics->max_y64 = max_y64;

done:
    av_free(line_buf);
    return ret;
}

static int draw_text(AVFilterContext *ctx, AVFrame *frame)
{
    DrawTextContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    FilterLink *inl = ff_filter_link(inlink);
    int x = 0, y = 0, ret;
    int shift_x64, shift_y64;
    int x64, y64;
    Glyph *glyph = NULL;

    time_t now = time(0);
    struct tm ltime;
    AVBPrint *bp = &s->expanded_text;

    FFDrawColor fontcolor;
    FFDrawColor shadowcolor;
    FFDrawColor bordercolor;
    FFDrawColor boxcolor;

    int width = frame->width;
    int height = frame->height;
    int rec_x = 0, rec_y = 0, rec_width = 0, rec_height = 0;
    int is_outside = 0;

    TextMetrics metrics;
    int line_height64;
    int preliminary_height;
    int first_visible_line, last_visible_line;

    av_bprint_clear(bp);

    if (s->basetime != AV_NOPTS_VALUE)
        now= frame->pts*av_q2d(ctx->inputs[0]->time_base) + s->basetime/1000000;

    switch (s->exp_mode) {
    case EXP_NONE:
        av_bprintf(bp, "%s", s->text);
        break;
    case EXP_NORMAL:
        if ((ret = ff_expand_text(&s->expand_text, s->text, &s->expanded_text)) < 0)
            return ret;
        break;
    case EXP_STRFTIME:
        localtime_r(&now, &ltime);
        av_bprint_strftime(bp, s->text, &ltime);
        break;
    }

    if (s->tc_opt_string) {
        char tcbuf[AV_TIMECODE_STR_SIZE];
        av_timecode_make_string(&s->tc, tcbuf, inl->frame_count_out);
        av_bprint_clear(bp);
        av_bprintf(bp, "%s%s", s->text, tcbuf);
    }

    if (!av_bprint_is_complete(bp))
        return AVERROR(ENOMEM);

    if (s->fontcolor_expr[0]) {
        /* If expression is set, evaluate and replace the static value */
        av_bprint_clear(&s->expanded_fontcolor);
        if ((ret = ff_expand_text(&s->expand_text, s->fontcolor_expr, &s->expanded_fontcolor)) < 0)
            return ret;
        if (!av_bprint_is_complete(&s->expanded_fontcolor))
            return AVERROR(ENOMEM);
        av_log(ctx, AV_LOG_DEBUG, "Evaluated fontcolor is '%s'\n", s->expanded_fontcolor.str);
        ret = av_parse_color(s->fontcolor.rgba, s->expanded_fontcolor.str, -1, s);
        if (ret)
            return ret;
        ff_draw_color(&s->dc, &s->fontcolor, s->fontcolor.rgba);
    }

    if ((ret = update_fontsize(ctx)) < 0) {
        return ret;
    }

    /* Phase 1: Count lines without shaping (fast) */
    line_height64 = s->face->size->metrics.height;

    /* Cache line boundaries across frames: only rebuild when text actually changed.
     * text_version tracks explicit reloads; lines_expanded_len catches EXP_NORMAL
     * content changes (e.g. %{pts} producing different-length output). */
    if (s->lines == NULL ||
        s->lines_text_version != s->text_version ||
        s->lines_expanded_len != s->expanded_text.len) {

        /* Free stale line data before rebuilding */
        if (s->lines) {
            for (int l = 0; l < s->line_count; l++) {
                av_freep(&s->lines[l].glyphs);
                hb_destroy(&s->lines[l].hb_data);
            }
            av_freep(&s->lines);
        }
        av_freep(&s->tab_clusters);

        /* First pass: just count lines and tabs */
        {
            const char *text = s->expanded_text.str;
            const uint8_t *p = (const uint8_t *)text;
            uint32_t code;
            int line_count = 0;
            int tab_count = 0;

            for (;;) {
                GET_UTF8(code, *p ? *p++ : 0, code = 0xfffd; goto continue_count1;);
continue_count1:
                if (ff_is_newline(code) || code == 0) {
                    ++line_count;
                    if (code == 0) break;
                } else if (code == '\t') {
                    ++tab_count;
                }
            }

            s->line_count = line_count;
            s->tab_count = tab_count;
        }

        /* Allocate lines and tab_clusters arrays */
        s->lines = av_mallocz(s->line_count * sizeof(TextLine));
        if (!s->lines)
            return AVERROR(ENOMEM);

        s->tab_clusters = av_mallocz(FFMAX(s->tab_count, 1) * sizeof(uint32_t));
        if (!s->tab_clusters) {
            av_freep(&s->lines);
            return AVERROR(ENOMEM);
        }

        /* Second pass: record line boundaries and tab positions */
        ret = count_text_lines(ctx, s->expanded_text.str, s->lines, s->line_count,
                               &s->line_count, &s->tab_count,
                               s->tab_clusters, s->tab_count);
        if (ret < 0) {
            av_freep(&s->lines);
            av_freep(&s->tab_clusters);
            return ret;
        }

        s->lines_text_version = s->text_version;
        s->lines_expanded_len = s->expanded_text.len;
        // Fresh lines array — no previous visible-line state is valid.
        s->prev_first_visible_line = -1;
        s->prev_last_visible_line  = -1;
    }
    /* else: text unchanged, reuse cached line boundaries */

    /* Calculate preliminary text height based on line count and font metrics */
    preliminary_height = POS_CEIL(line_height64 * s->line_count, 64);

    /* Set preliminary VAR_TH for canvas expression evaluation */
    s->var_values[VAR_TH] = s->var_values[VAR_TEXT_H] = preliminary_height;
    s->var_values[VAR_LINE_H] = s->var_values[VAR_LH] = line_height64 / 64.;
    s->var_values[VAR_FONT_A] = s->face->size->metrics.ascender / 64;
    s->var_values[VAR_FONT_D] = -s->face->size->metrics.descender / 64;

    /* Evaluate canvas expressions EARLY (before expensive shaping) */
    s->canvas_w = av_expr_eval(s->canvas_w_pexpr, s->var_values, &s->prng);
    s->canvas_h = av_expr_eval(s->canvas_h_pexpr, s->var_values, &s->prng);
    if (s->canvas_w > 0 && s->canvas_h > 0) {
        s->canvas_x = av_expr_eval(s->canvas_x_pexpr, s->var_values, &s->prng);
        s->canvas_y = av_expr_eval(s->canvas_y_pexpr, s->var_values, &s->prng);
    }

    /* Calculate visible lines for viewport optimization */
    /* Disable optimization for canvas_tile mode (all lines needed for tiling) */
    if (s->canvas_tile || s->canvas_h <= 0) {
        first_visible_line = 0;
        last_visible_line = s->line_count - 1;
    } else {
        calculate_visible_lines(s->line_count, line_height64,
                               s->line_spacing, s->canvas_y, s->canvas_h,
                               2, /* buffer_lines */
                               &first_visible_line, &last_visible_line);
    }

    av_log(ctx, AV_LOG_DEBUG, "Viewport optimization: lines %d-%d of %d visible (canvas_y=%d, canvas_h=%d)\n",
           first_visible_line, last_visible_line, s->line_count, s->canvas_y, s->canvas_h);

    /* Phase 2: Shape only visible lines */
    if ((ret = measure_text(ctx, &metrics, first_visible_line, last_visible_line)) < 0) {
        return ret;
    }

    s->max_glyph_h = POS_CEIL(metrics.max_y64 - metrics.min_y64, 64);
    s->max_glyph_w = POS_CEIL(metrics.max_x64 - metrics.min_x64, 64);

    s->var_values[VAR_TW] = s->var_values[VAR_TEXT_W] = metrics.width;
    s->var_values[VAR_TH] = s->var_values[VAR_TEXT_H] = metrics.height;

    s->var_values[VAR_MAX_GLYPH_W] = s->max_glyph_w;
    s->var_values[VAR_MAX_GLYPH_H] = s->max_glyph_h;
    s->var_values[VAR_MAX_GLYPH_A] = s->var_values[VAR_ASCENT] = POS_CEIL(metrics.max_y64, 64);
    s->var_values[VAR_FONT_A] = s->face->size->metrics.ascender / 64;
    s->var_values[VAR_MAX_GLYPH_D] = s->var_values[VAR_DESCENT] = POS_CEIL(metrics.min_y64, 64);
    s->var_values[VAR_FONT_D] = -s->face->size->metrics.descender / 64;

    s->var_values[VAR_TOP_A] = POS_CEIL(metrics.offset_top64, 64);
    s->var_values[VAR_BOTTOM_D] = -POS_CEIL(metrics.offset_bottom64, 64);
    s->var_values[VAR_LINE_H] = s->var_values[VAR_LH] = metrics.line_height64 / 64.;

    if (s->text_source == AV_FRAME_DATA_DETECTION_BBOXES) {
        s->var_values[VAR_X] = s->x;
        s->var_values[VAR_Y] = s->y;
    } else {
        s->x = s->var_values[VAR_X] = av_expr_eval(s->x_pexpr, s->var_values, &s->prng);
        double prev_y = s->var_values[VAR_Y];
        s->y = s->var_values[VAR_Y] = av_expr_eval(s->y_pexpr, s->var_values, &s->prng);
        /* Re-evaluate x only if y changed, in case x is expressed from y */
        if (s->var_values[VAR_Y] != prev_y)
            s->x = s->var_values[VAR_X] = av_expr_eval(s->x_pexpr, s->var_values, &s->prng);
    }

    /* Re-evaluate canvas expressions with final metrics if needed */
    if (s->canvas_w <= 0 || s->canvas_h <= 0) {
        s->canvas_w = av_expr_eval(s->canvas_w_pexpr, s->var_values, &s->prng);
        s->canvas_h = av_expr_eval(s->canvas_h_pexpr, s->var_values, &s->prng);
        if (s->canvas_w > 0 && s->canvas_h > 0) {
            s->canvas_x = av_expr_eval(s->canvas_x_pexpr, s->var_values, &s->prng);
            s->canvas_y = av_expr_eval(s->canvas_y_pexpr, s->var_values, &s->prng);
        }
    }

    update_alpha(s);
    update_color_with_alpha(s, &fontcolor  , s->fontcolor  );
    update_color_with_alpha(s, &shadowcolor, s->shadowcolor);
    update_color_with_alpha(s, &bordercolor, s->bordercolor);
    update_color_with_alpha(s, &boxcolor   , s->boxcolor   );

    if (s->draw_box && s->boxborderw) {
        int bbsize[4];
        int count;
        count = string_to_array(s->boxborderw, bbsize, 4);
        if (count == 1) {
            s->bb_top = s->bb_right = s->bb_bottom = s->bb_left = bbsize[0];
        } else if (count == 2) {
            s->bb_top = s->bb_bottom = bbsize[0];
            s->bb_right = s->bb_left = bbsize[1];
        } else if (count == 3) {
            s->bb_top = bbsize[0];
            s->bb_right = s->bb_left = bbsize[1];
            s->bb_bottom = bbsize[2];
        } else if (count == 4) {
            s->bb_top = bbsize[0];
            s->bb_right = bbsize[1];
            s->bb_bottom = bbsize[2];
            s->bb_left = bbsize[3];
        }
    } else {
        s->bb_top = s->bb_right = s->bb_bottom = s->bb_left = 0;
    }

    if (s->fix_bounds) {
        /* calculate footprint of text effects */
        int borderoffset  = s->borderw  ? FFMAX(s->borderw, 0) : 0;

        int offsetleft = FFMAX3(FFMAX(s->bb_left, 0), borderoffset,
                                (s->shadowx < 0 ? FFABS(s->shadowx) : 0));
        int offsettop = FFMAX3(FFMAX(s->bb_top, 0), borderoffset,
                                (s->shadowy < 0 ? FFABS(s->shadowy) : 0));
        int offsetright = FFMAX3(FFMAX(s->bb_right, 0), borderoffset,
                                 (s->shadowx > 0 ? s->shadowx : 0));
        int offsetbottom = FFMAX3(FFMAX(s->bb_bottom, 0), borderoffset,
                                  (s->shadowy > 0 ? s->shadowy : 0));

        if (s->x - offsetleft < 0) s->x = offsetleft;
        if (s->y - offsettop < 0)  s->y = offsettop;

        if (s->x + metrics.width + offsetright > width)
            s->x = FFMAX(width - metrics.width - offsetright, 0);
        if (s->y + metrics.height + offsetbottom > height)
            s->y = FFMAX(height - metrics.height - offsetbottom, 0);
    }

    x = 0;
    y = 0;
    x64 = (int)(s->x * 64.);
    if (s->y_align == YA_FONT) {
        y64 = (int)(s->y * 64. + s->face->size->metrics.ascender);
    } else if (s->y_align == YA_BASELINE) {
        y64 = (int)(s->y * 64.);
    } else {
        y64 = (int)(s->y * 64. + metrics.offset_top64);
    }

    // Jump directly to first visible line instead of iterating from line 0.
    // For uniform line height (the common case) y is strictly additive so
    // we can precompute its value without touching every line struct.
    y = first_visible_line * (metrics.line_height64 + s->line_spacing * 64);

    for (int l = first_visible_line; l <= last_visible_line; ++l) {
        TextLine *line = &s->lines[l];
        HarfbuzzData *hb = &line->hb_data;

        if (!line->is_shaped) {
            y += metrics.line_height64 + s->line_spacing * 64;
            continue;
        }

        // Issue 5: Reuse existing allocation when glyph count hasn't grown.
        if (hb->glyph_count > (unsigned int)line->glyphs_alloc) {
            av_freep(&line->glyphs);
            line->glyphs = av_mallocz(hb->glyph_count * sizeof(GlyphInfo));
            if (!line->glyphs)
                return AVERROR(ENOMEM);
            line->glyphs_alloc = hb->glyph_count;
        }

        // Issue 7: Use pre-computed per-line tab range instead of global last_tab_idx.
        int line_tab_idx = line->first_tab_idx;
        int tab_end_idx  = line->first_tab_idx + line->line_tab_count;
        for (int t = 0; t < (int)hb->glyph_count; ++t) {
            GlyphInfo *g_info = &line->glyphs[t];
            uint8_t is_tab = line_tab_idx < tab_end_idx &&
                hb->glyph_info[t].cluster == s->tab_clusters[line_tab_idx] - line->cluster_offset;
            int true_x, true_y;
            if (is_tab) {
                ++line_tab_idx;
            }
            true_x = x + hb->glyph_pos[t].x_offset;
            true_y = y + hb->glyph_pos[t].y_offset;
            shift_x64 = (((x64 + true_x) >> 4) & 0b0011) << 4;
            shift_y64 = ((4 - (((y64 + true_y) >> 4) & 0b0011)) & 0b0011) << 4;

            ret = load_glyph(ctx, &glyph, hb->glyph_info[t].codepoint, shift_x64, shift_y64);
            if (ret != 0) {
                return ret;
            }
            g_info->code = hb->glyph_info[t].codepoint;
            g_info->x = (x64 + true_x) >> 6;
            g_info->y = ((y64 + true_y) >> 6) + (shift_y64 > 0 ? 1 : 0);
            g_info->shift_x64 = shift_x64;
            g_info->shift_y64 = shift_y64;

            if (!is_tab) {
                x += hb->glyph_pos[t].x_advance;
            } else {
                int size = s->blank_advance64 * s->tabsize;
                x = (x / size + 1) * size;
            }
            y += hb->glyph_pos[t].y_advance;
        }

        y += metrics.line_height64 + s->line_spacing * 64;
        x = 0;
    }

    metrics.rect_x = s->x;
    if (s->y_align == YA_BASELINE) {
        metrics.rect_y = s->y - metrics.offset_top64 / 64;
    } else {
        metrics.rect_y = s->y;
    }

    s->box_width = s->boxw == 0 ? metrics.width : s->boxw;
    s->box_height = s->boxh == 0 ? metrics.height : s->boxh;

    if (!s->draw_box) {
        // Create a border for the clipping region to take into account subpixel
        // errors in text measurement and effects.
        int borderoffset = s->borderw ? FFMAX(s->borderw, 0) : 0;
        s->bb_left = borderoffset + (s->shadowx < 0 ? FFABS(s->shadowx) : 0) + 1;
        s->bb_top = borderoffset + (s->shadowy < 0 ? FFABS(s->shadowy) : 0) + 1;
        s->bb_right = borderoffset + (s->shadowx > 0 ? s->shadowx : 0) + 1;
        s->bb_bottom = borderoffset + (s->shadowy > 0 ? s->shadowy : 0) + 1;
    }

    /* Check if the whole box is out of the frame */
    {
        int check_width = (s->canvas_w > 0 && s->canvas_h > 0) ? s->canvas_w : s->box_width;
        int check_height = (s->canvas_w > 0 && s->canvas_h > 0) ? s->canvas_h : s->box_height;
        is_outside = metrics.rect_x - s->bb_left >= width ||
                        metrics.rect_y - s->bb_top >= height ||
                        metrics.rect_x + check_width + s->bb_right <= 0 ||
                        metrics.rect_y + check_height + s->bb_bottom <= 0;
    }

    if (!is_outside) {
        /* draw box */
        if (s->draw_box) {
            rec_x = metrics.rect_x - s->bb_left;
            rec_y = metrics.rect_y - s->bb_top;
            if (s->canvas_w > 0 && s->canvas_h > 0) {
                rec_width = s->canvas_w + s->bb_right + s->bb_left;
                rec_height = s->canvas_h + s->bb_bottom + s->bb_top;
            } else {
                rec_width = s->box_width + s->bb_right + s->bb_left;
                rec_height = s->box_height + s->bb_bottom + s->bb_top;
            }
            ff_blend_rectangle(&s->dc, &boxcolor,
                frame->data, frame->linesize, width, height,
                rec_x, rec_y, rec_width, rec_height);
        }

        if (s->shadowx || s->shadowy) {
            if ((ret = draw_glyphs(ctx, frame, &shadowcolor, &metrics,
                    s->shadowx, s->shadowy, s->borderw,
                    s->canvas_x, s->canvas_y, s->canvas_w, s->canvas_h,
                    metrics.width, metrics.height, s->canvas_tile)) < 0) {
                return ret;
            }
        }

        if (s->borderw) {
            if ((ret = draw_glyphs(ctx, frame, &bordercolor, &metrics,
                    0, 0, s->borderw,
                    s->canvas_x, s->canvas_y, s->canvas_w, s->canvas_h,
                    metrics.width, metrics.height, s->canvas_tile)) < 0) {
                return ret;
            }
        }

        if ((ret = draw_glyphs(ctx, frame, &fontcolor, &metrics, 0, 0, 0,
                s->canvas_x, s->canvas_y, s->canvas_w, s->canvas_h,
                metrics.width, metrics.height, s->canvas_tile)) < 0) {
            return ret;
        }
    }

    // Issue 5: glyphs[] and hb_data.buf are kept alive across frames to avoid
    // per-frame malloc/free churn. They are freed when text changes (rebuild
    // path in draw_text) or at uninit time.

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    FilterLink *inl = ff_filter_link(inlink);
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    DrawTextContext *s = ctx->priv;
    int ret;
    const AVDetectionBBoxHeader *header = NULL;
    const AVDetectionBBox *bbox;
    AVFrameSideData *sd;
    int loop = 1;

    if (s->text_source == AV_FRAME_DATA_DETECTION_BBOXES) {
        sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DETECTION_BBOXES);
        if (sd) {
            header = (AVDetectionBBoxHeader *)sd->data;
            loop = header->nb_bboxes;
        } else {
            av_log(ctx, AV_LOG_WARNING, "No detection bboxes.\n");
            return ff_filter_frame(outlink, frame);
        }
    }

    if (s->reload && !(inl->frame_count_out % s->reload)) {
        if ((ret = ff_load_textfile(ctx, (const char *)s->textfile, &s->text, NULL)) < 0) {
            av_frame_free(&frame);
            return ret;
        }
        s->text_version++;
#if CONFIG_LIBFRIBIDI
        if (s->text_shaping)
            if ((ret = shape_text(ctx)) < 0) {
                av_frame_free(&frame);
                return ret;
            }
#endif
    }

    s->var_values[VAR_N] = inl->frame_count_out + s->start_number;
    s->var_values[VAR_T] = frame->pts == AV_NOPTS_VALUE ?
        NAN : frame->pts * av_q2d(inlink->time_base);

    s->var_values[VAR_PICT_TYPE] = frame->pict_type;
    s->var_values[VAR_DURATION] = frame->duration * av_q2d(inlink->time_base);

    s->metadata = frame->metadata;

    for (int i = 0; i < loop; i++) {
        if (header) {
            bbox = av_get_detection_bbox(header, i);
            size_t text_buf_size = (size_t)(AV_DETECTION_BBOX_LABEL_NAME_MAX_SIZE + 1) *
                                   (AV_NUM_DETECTION_BBOX_CLASSIFY + 1);
            av_strlcpy(s->text, bbox->detect_label, text_buf_size);
            for (int j = 0; j < bbox->classify_count; j++) {
                av_strlcat(s->text, ", ", text_buf_size);
                av_strlcat(s->text, bbox->classify_labels[j], text_buf_size);
            }
            s->x = bbox->x;
            s->y = bbox->y - s->fontsize;
            s->text_version++;
        }

        // Reload text file when canvas_tile_reload is enabled and file has changed.
        // Throttle stat() to once per 30 frames to avoid per-frame syscall overhead.
        if (s->canvas_tile_reload && s->textfile && !(inl->frame_count_out % 30)) {
            struct stat file_stat;
            if (stat(s->textfile, &file_stat) == 0 &&
                file_stat.st_mtime != s->canvas_tile_reload_mtime) {
                s->canvas_tile_reload_mtime = file_stat.st_mtime;
                if ((ret = ff_load_textfile(ctx, (const char *)s->textfile, &s->text, NULL)) < 0) {
                    av_frame_free(&frame);
                    return ret;
                }
                s->text_version++;
#if CONFIG_LIBFRIBIDI
                if (s->text_shaping)
                    if ((ret = shape_text(ctx)) < 0) {
                        av_frame_free(&frame);
                        return ret;
                    }
#endif
            }
        }

        draw_text(ctx, frame);
    }

    return ff_filter_frame(outlink, frame);
}

static const AVFilterPad avfilter_vf_drawtext_inputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_VIDEO,
        .flags          = AVFILTERPAD_FLAG_NEEDS_WRITABLE,
        .filter_frame   = filter_frame,
        .config_props   = config_input,
    },
};

const FFFilter ff_vf_drawtext = {
    .p.name        = "drawtext",
    .p.description = NULL_IF_CONFIG_SMALL("Draw text on top of video frames using libfreetype library."),
    .p.priv_class  = &drawtext_class,
    .p.flags       = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    .priv_size     = sizeof(DrawTextContext),
    .init          = init,
    .uninit        = uninit,
    FILTER_INPUTS(avfilter_vf_drawtext_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
    FILTER_QUERY_FUNC2(query_formats),
    .process_command = command,
};
