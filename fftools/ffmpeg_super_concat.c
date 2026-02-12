/*
 * Super Concat - JSON playlist-driven concatenation with per-segment filtergraphs
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "ffmpeg_super_concat.h"

#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersrc.h"
#include "libavfilter/buffersink.h"
#include "libavutil/avutil.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/channel_layout.h"
#include "libavutil/mathematics.h"
#include "libavutil/timestamp.h"
#include "libavutil/avstring.h"
#include "libavutil/mem.h"

/* ======================================================================
 * Minimal JSON Parser
 * ====================================================================== */

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JSONType;

typedef struct JSONValue JSONValue;

typedef struct JSONPair {
    char *key;
    JSONValue *value;
} JSONPair;

struct JSONValue {
    JSONType type;
    union {
        int bool_val;
        double num_val;
        char *str_val;
        struct {
            JSONValue **items;
            int count;
        } array;
        struct {
            JSONPair *pairs;
            int count;
        } object;
    };
};

static void json_free(JSONValue *v);

static void json_free(JSONValue *v)
{
    if (!v)
        return;
    switch (v->type) {
    case JSON_STRING:
        av_free(v->str_val);
        break;
    case JSON_ARRAY:
        for (int i = 0; i < v->array.count; i++)
            json_free(v->array.items[i]);
        av_free(v->array.items);
        break;
    case JSON_OBJECT:
        for (int i = 0; i < v->object.count; i++) {
            av_free(v->object.pairs[i].key);
            json_free(v->object.pairs[i].value);
        }
        av_free(v->object.pairs);
        break;
    default:
        break;
    }
    av_free(v);
}

static const char *json_skip_ws(const char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

static JSONValue *json_parse_value(const char **s);

static JSONValue *json_parse_string(const char **s)
{
    const char *p = *s;
    if (*p != '"')
        return NULL;
    p++;

    /* estimate length */
    const char *start = p;
    while (*p && *p != '"') {
        if (*p == '\\')
            p++;
        p++;
    }
    if (*p != '"')
        return NULL;

    int len = p - start;
    char *str = av_malloc(len + 1);
    if (!str)
        return NULL;

    /* copy with escape handling */
    int j = 0;
    p = start;
    while (*p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"':  str[j++] = '"';  break;
            case '\\': str[j++] = '\\'; break;
            case '/':  str[j++] = '/';  break;
            case 'n':  str[j++] = '\n'; break;
            case 't':  str[j++] = '\t'; break;
            case 'r':  str[j++] = '\r'; break;
            default:   str[j++] = *p;   break;
            }
        } else {
            str[j++] = *p;
        }
        p++;
    }
    str[j] = '\0';
    p++; /* skip closing quote */

    JSONValue *v = av_mallocz(sizeof(*v));
    if (!v) {
        av_free(str);
        return NULL;
    }
    v->type = JSON_STRING;
    v->str_val = str;
    *s = p;
    return v;
}

static JSONValue *json_parse_number(const char **s)
{
    char *end;
    double val = strtod(*s, &end);
    if (end == *s)
        return NULL;
    JSONValue *v = av_mallocz(sizeof(*v));
    if (!v)
        return NULL;
    v->type = JSON_NUMBER;
    v->num_val = val;
    *s = end;
    return v;
}

static JSONValue *json_parse_array(const char **s)
{
    const char *p = *s;
    if (*p != '[')
        return NULL;
    p++;

    JSONValue *v = av_mallocz(sizeof(*v));
    if (!v)
        return NULL;
    v->type = JSON_ARRAY;

    p = json_skip_ws(p);
    if (*p == ']') {
        *s = p + 1;
        return v;
    }

    while (1) {
        p = json_skip_ws(p);
        JSONValue *item = json_parse_value(&p);
        if (!item) {
            json_free(v);
            return NULL;
        }

        JSONValue **new_items = av_realloc_array(v->array.items,
                                                  v->array.count + 1,
                                                  sizeof(*v->array.items));
        if (!new_items) {
            json_free(item);
            json_free(v);
            return NULL;
        }
        v->array.items = new_items;
        v->array.items[v->array.count++] = item;

        p = json_skip_ws(p);
        if (*p == ']') {
            p++;
            break;
        }
        if (*p != ',') {
            json_free(v);
            return NULL;
        }
        p++;
    }

    *s = p;
    return v;
}

static JSONValue *json_parse_object(const char **s)
{
    const char *p = *s;
    if (*p != '{')
        return NULL;
    p++;

    JSONValue *v = av_mallocz(sizeof(*v));
    if (!v)
        return NULL;
    v->type = JSON_OBJECT;

    p = json_skip_ws(p);
    if (*p == '}') {
        *s = p + 1;
        return v;
    }

    while (1) {
        p = json_skip_ws(p);

        /* parse key */
        JSONValue *key_val = json_parse_string(&p);
        if (!key_val) {
            json_free(v);
            return NULL;
        }
        char *key = key_val->str_val;
        key_val->str_val = NULL;
        json_free(key_val);

        p = json_skip_ws(p);
        if (*p != ':') {
            av_free(key);
            json_free(v);
            return NULL;
        }
        p++;
        p = json_skip_ws(p);

        JSONValue *val = json_parse_value(&p);
        if (!val) {
            av_free(key);
            json_free(v);
            return NULL;
        }

        JSONPair *new_pairs = av_realloc_array(v->object.pairs,
                                                v->object.count + 1,
                                                sizeof(*v->object.pairs));
        if (!new_pairs) {
            av_free(key);
            json_free(val);
            json_free(v);
            return NULL;
        }
        v->object.pairs = new_pairs;
        v->object.pairs[v->object.count].key = key;
        v->object.pairs[v->object.count].value = val;
        v->object.count++;

        p = json_skip_ws(p);
        if (*p == '}') {
            p++;
            break;
        }
        if (*p != ',') {
            json_free(v);
            return NULL;
        }
        p++;
    }

    *s = p;
    return v;
}

static JSONValue *json_parse_value(const char **s)
{
    const char *p = json_skip_ws(*s);
    *s = p;

    if (*p == '"')
        return json_parse_string(s);
    if (*p == '[')
        return json_parse_array(s);
    if (*p == '{')
        return json_parse_object(s);
    if (*p == 't' && !strncmp(p, "true", 4)) {
        JSONValue *v = av_mallocz(sizeof(*v));
        if (!v) return NULL;
        v->type = JSON_BOOL;
        v->bool_val = 1;
        *s = p + 4;
        return v;
    }
    if (*p == 'f' && !strncmp(p, "false", 5)) {
        JSONValue *v = av_mallocz(sizeof(*v));
        if (!v) return NULL;
        v->type = JSON_BOOL;
        v->bool_val = 0;
        *s = p + 5;
        return v;
    }
    if (*p == 'n' && !strncmp(p, "null", 4)) {
        JSONValue *v = av_mallocz(sizeof(*v));
        if (!v) return NULL;
        v->type = JSON_NULL;
        *s = p + 4;
        return v;
    }
    if (*p == '-' || isdigit((unsigned char)*p))
        return json_parse_number(s);

    return NULL;
}

static JSONValue *json_parse(const char *text)
{
    const char *s = text;
    return json_parse_value(&s);
}

/* Helper to find a key in a JSON object */
static JSONValue *json_object_get(const JSONValue *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJECT)
        return NULL;
    for (int i = 0; i < obj->object.count; i++) {
        if (!strcmp(obj->object.pairs[i].key, key))
            return obj->object.pairs[i].value;
    }
    return NULL;
}

/* ======================================================================
 * Playlist Parser
 * ====================================================================== */

static void sc_playlist_free(SCPlaylist *pl)
{
    if (!pl)
        return;
    for (int i = 0; i < pl->nb_segments; i++) {
        SCSegment *seg = &pl->segments[i];
        for (int j = 0; j < seg->nb_inputs; j++) {
            av_free(seg->inputs[j].url);
            av_dict_free(&seg->inputs[j].options);
        }
        av_free(seg->inputs);
        av_free(seg->filter);
        for (int j = 0; j < seg->nb_maps; j++)
            av_free(seg->maps[j]);
        av_free(seg->maps);
    }
    av_free(pl->segments);
    av_freep(&pl);
}

static SCPlaylist *sc_playlist_parse(const char *json_text)
{
    JSONValue *root = json_parse(json_text);
    if (!root || root->type != JSON_ARRAY) {
        av_log(NULL, AV_LOG_ERROR, "super_concat: playlist must be a JSON array\n");
        json_free(root);
        return NULL;
    }

    SCPlaylist *pl = av_mallocz(sizeof(*pl));
    if (!pl) {
        json_free(root);
        return NULL;
    }

    pl->nb_segments = root->array.count;
    pl->segments = av_calloc(pl->nb_segments, sizeof(*pl->segments));
    if (!pl->segments) {
        av_free(pl);
        json_free(root);
        return NULL;
    }

    for (int i = 0; i < pl->nb_segments; i++) {
        JSONValue *seg_json = root->array.items[i];
        if (seg_json->type != JSON_OBJECT) {
            av_log(NULL, AV_LOG_ERROR, "super_concat: segment %d must be an object\n", i);
            goto fail;
        }

        SCSegment *seg = &pl->segments[i];

        /* parse inputs */
        JSONValue *inputs_json = json_object_get(seg_json, "inputs");
        if (!inputs_json || inputs_json->type != JSON_ARRAY || inputs_json->array.count < 1) {
            av_log(NULL, AV_LOG_ERROR, "super_concat: segment %d must have an 'inputs' array\n", i);
            goto fail;
        }

        seg->nb_inputs = inputs_json->array.count;
        seg->inputs = av_calloc(seg->nb_inputs, sizeof(*seg->inputs));
        if (!seg->inputs)
            goto fail;

        for (int j = 0; j < seg->nb_inputs; j++) {
            JSONValue *inp = inputs_json->array.items[j];
            if (inp->type != JSON_OBJECT) {
                av_log(NULL, AV_LOG_ERROR, "super_concat: segment %d input %d must be an object\n", i, j);
                goto fail;
            }

            JSONValue *url_val = json_object_get(inp, "url");
            if (!url_val || url_val->type != JSON_STRING) {
                av_log(NULL, AV_LOG_ERROR, "super_concat: segment %d input %d must have a 'url' string\n", i, j);
                goto fail;
            }
            seg->inputs[j].url = av_strdup(url_val->str_val);

            JSONValue *opts = json_object_get(inp, "options");
            if (opts && opts->type == JSON_OBJECT) {
                for (int k = 0; k < opts->object.count; k++) {
                    const char *okey = opts->object.pairs[k].key;
                    JSONValue *oval = opts->object.pairs[k].value;
                    char val_buf[64];

                    /* strip leading dash */
                    if (okey[0] == '-')
                        okey++;

                    if (oval->type == JSON_STRING) {
                        av_dict_set(&seg->inputs[j].options, okey, oval->str_val, 0);
                    } else if (oval->type == JSON_NUMBER) {
                        snprintf(val_buf, sizeof(val_buf), "%g", oval->num_val);
                        av_dict_set(&seg->inputs[j].options, okey, val_buf, 0);
                    }
                }
            }
        }

        /* parse filter */
        JSONValue *filter_val = json_object_get(seg_json, "filter");
        if (filter_val && filter_val->type == JSON_STRING)
            seg->filter = av_strdup(filter_val->str_val);

        /* parse maps */
        JSONValue *maps_val = json_object_get(seg_json, "maps");
        if (maps_val && maps_val->type == JSON_ARRAY) {
            seg->nb_maps = maps_val->array.count;
            seg->maps = av_calloc(seg->nb_maps, sizeof(*seg->maps));
            if (!seg->maps)
                goto fail;
            for (int j = 0; j < seg->nb_maps; j++) {
                if (maps_val->array.items[j]->type != JSON_STRING) {
                    av_log(NULL, AV_LOG_ERROR, "super_concat: segment %d map %d must be a string\n", i, j);
                    goto fail;
                }
                seg->maps[j] = av_strdup(maps_val->array.items[j]->str_val);
            }
        }
    }

    json_free(root);
    return pl;

fail:
    json_free(root);
    sc_playlist_free(pl);
    return NULL;
}

/* ======================================================================
 * Output Stream Info
 * ====================================================================== */

typedef struct SCOutputStream {
    enum AVMediaType type;
    /* video params */
    int width, height;
    enum AVPixelFormat pix_fmt;
    AVRational frame_rate;
    AVRational sample_aspect_ratio;
    /* audio params */
    int sample_rate;
    AVChannelLayout ch_layout;
    enum AVSampleFormat sample_fmt;
    /* encoder */
    AVCodecContext *enc_ctx;
    int stream_index;       /* index in output AVFormatContext */
    /* timestamp tracking */
    int64_t ts_offset;      /* cumulative PTS offset in encoder timebase */
    int64_t segment_base_pts;
    int64_t last_pts;
    int64_t last_duration;
    int got_first_pts;
} SCOutputStream;

/* ======================================================================
 * Parse output codec options from argv
 * ====================================================================== */

typedef struct SCOutputOpts {
    const char *vcodec;
    const char *acodec;
    AVDictionary *vopts;
    AVDictionary *aopts;
    const char *format;
} SCOutputOpts;

static void sc_output_opts_free(SCOutputOpts *opts)
{
    av_dict_free(&opts->vopts);
    av_dict_free(&opts->aopts);
}

static void sc_parse_output_opts(int argc, char **argv, SCOutputOpts *opts)
{
    memset(opts, 0, sizeof(*opts));

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-c:v") && i + 1 < argc) {
            opts->vcodec = argv[++i];
        } else if (!strcmp(argv[i], "-c:a") && i + 1 < argc) {
            opts->acodec = argv[++i];
        } else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            opts->format = argv[++i];
        } else if (!strcmp(argv[i], "-b:v") && i + 1 < argc) {
            av_dict_set(&opts->vopts, "b", argv[++i], 0);
        } else if (!strcmp(argv[i], "-b:a") && i + 1 < argc) {
            av_dict_set(&opts->aopts, "b", argv[++i], 0);
        } else if (!strcmp(argv[i], "-crf") && i + 1 < argc) {
            av_dict_set(&opts->vopts, "crf", argv[++i], 0);
        } else if (!strcmp(argv[i], "-preset") && i + 1 < argc) {
            av_dict_set(&opts->vopts, "preset", argv[++i], 0);
        } else if (!strcmp(argv[i], "-g") && i + 1 < argc) {
            av_dict_set(&opts->vopts, "g", argv[++i], 0);
        } else if (!strcmp(argv[i], "-ar") && i + 1 < argc) {
            av_dict_set(&opts->aopts, "ar", argv[++i], 0);
        } else if (!strcmp(argv[i], "-ac") && i + 1 < argc) {
            av_dict_set(&opts->aopts, "ac", argv[++i], 0);
        } else if (!strcmp(argv[i], "-profile:v") && i + 1 < argc) {
            av_dict_set(&opts->vopts, "profile", argv[++i], 0);
        } else if (!strcmp(argv[i], "-pix_fmt") && i + 1 < argc) {
            av_dict_set(&opts->vopts, "pix_fmt", argv[++i], 0);
        } else if (!strcmp(argv[i], "-vendor") && i + 1 < argc) {
            av_dict_set(&opts->vopts, "vendor", argv[++i], 0);
        } else if (!strcmp(argv[i], "-qscale:v") && i + 1 < argc) {
            av_dict_set(&opts->vopts, "qscale", argv[++i], 0);
        } else if (!strcmp(argv[i], "-tag:v") && i + 1 < argc) {
            av_dict_set(&opts->vopts, "tag", argv[++i], 0);
        }
    }
}

/* ======================================================================
 * Segment Processing Context
 * ====================================================================== */

typedef struct SCSegmentCtx {
    AVFormatContext **fmt_ctxs;
    AVCodecContext **dec_ctxs;   /* one per input stream across all inputs */
    int *dec_stream_input;      /* which input file each decoder belongs to */
    int nb_decoders;

    AVFilterGraph *filter_graph;
    AVFilterContext **buffersrc_ctxs;
    AVFilterContext **buffersink_ctxs;
    int nb_buffersrcs;
    int nb_buffersinks;

    /* per-input tracking */
    int *input_eof;
    int64_t *input_last_dts;
    int64_t *input_duration_limit; /* in AV_TIME_BASE units, or AV_NOPTS_VALUE */
    int64_t *input_start_time;     /* start time after seek */
    int nb_inputs;
} SCSegmentCtx;

static void sc_segment_ctx_free(SCSegmentCtx *ctx)
{
    if (!ctx)
        return;

    if (ctx->dec_ctxs) {
        for (int i = 0; i < ctx->nb_decoders; i++)
            avcodec_free_context(&ctx->dec_ctxs[i]);
        av_free(ctx->dec_ctxs);
    }
    av_free(ctx->dec_stream_input);

    if (ctx->fmt_ctxs) {
        for (int i = 0; i < ctx->nb_inputs; i++)
            avformat_close_input(&ctx->fmt_ctxs[i]);
        av_free(ctx->fmt_ctxs);
    }

    avfilter_graph_free(&ctx->filter_graph);
    av_free(ctx->buffersrc_ctxs);
    av_free(ctx->buffersink_ctxs);
    av_free(ctx->input_eof);
    av_free(ctx->input_last_dts);
    av_free(ctx->input_duration_limit);
    av_free(ctx->input_start_time);
}

/* ======================================================================
 * Open inputs and decoders for a segment
 * ====================================================================== */

static int sc_open_inputs(SCSegment *seg, SCSegmentCtx *ctx)
{
    int ret;

    ctx->nb_inputs = seg->nb_inputs;
    ctx->fmt_ctxs = av_calloc(ctx->nb_inputs, sizeof(*ctx->fmt_ctxs));
    ctx->input_eof = av_calloc(ctx->nb_inputs, sizeof(*ctx->input_eof));
    ctx->input_last_dts = av_calloc(ctx->nb_inputs, sizeof(*ctx->input_last_dts));
    ctx->input_duration_limit = av_calloc(ctx->nb_inputs, sizeof(*ctx->input_duration_limit));
    ctx->input_start_time = av_calloc(ctx->nb_inputs, sizeof(*ctx->input_start_time));
    if (!ctx->fmt_ctxs || !ctx->input_eof || !ctx->input_last_dts ||
        !ctx->input_duration_limit || !ctx->input_start_time)
        return AVERROR(ENOMEM);

    for (int i = 0; i < ctx->nb_inputs; i++) {
        ctx->input_last_dts[i] = AV_NOPTS_VALUE;
        ctx->input_duration_limit[i] = AV_NOPTS_VALUE;
        ctx->input_start_time[i] = AV_NOPTS_VALUE;
    }

    /* count total streams for decoder allocation */
    int total_streams = 0;

    for (int i = 0; i < seg->nb_inputs; i++) {
        AVDictionary *format_opts = NULL;
        const AVDictionaryEntry *e;

        /* separate format options from our custom options */
        const AVDictionaryEntry *ss_entry = av_dict_get(seg->inputs[i].options, "ss", NULL, 0);
        const AVDictionaryEntry *t_entry = av_dict_get(seg->inputs[i].options, "t", NULL, 0);

        /* copy non-ss/t options to format_opts */
        e = NULL;
        while ((e = av_dict_iterate(seg->inputs[i].options, e))) {
            if (strcmp(e->key, "ss") && strcmp(e->key, "t"))
                av_dict_set(&format_opts, e->key, e->value, 0);
        }

        ret = avformat_open_input(&ctx->fmt_ctxs[i], seg->inputs[i].url,
                                   NULL, &format_opts);
        av_dict_free(&format_opts);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "super_concat: failed to open input '%s': %s\n",
                   seg->inputs[i].url, av_err2str(ret));
            return ret;
        }

        ret = avformat_find_stream_info(ctx->fmt_ctxs[i], NULL);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "super_concat: failed to find stream info for '%s'\n",
                   seg->inputs[i].url);
            return ret;
        }

        /* handle -ss (seek) */
        if (ss_entry) {
            int64_t seek_ts;
            double ss_val = atof(ss_entry->value);
            seek_ts = (int64_t)(ss_val * AV_TIME_BASE);
            ctx->input_start_time[i] = seek_ts;
            ret = avformat_seek_file(ctx->fmt_ctxs[i], -1, INT64_MIN, seek_ts, seek_ts, 0);
            if (ret < 0)
                av_log(NULL, AV_LOG_WARNING, "super_concat: seek failed for '%s'\n",
                       seg->inputs[i].url);
        }

        /* handle -t (duration) */
        if (t_entry) {
            double t_val = atof(t_entry->value);
            ctx->input_duration_limit[i] = (int64_t)(t_val * AV_TIME_BASE);
        }

        total_streams += ctx->fmt_ctxs[i]->nb_streams;
    }

    /* allocate decoders for all streams */
    ctx->nb_decoders = total_streams;
    ctx->dec_ctxs = av_calloc(total_streams, sizeof(*ctx->dec_ctxs));
    ctx->dec_stream_input = av_calloc(total_streams, sizeof(*ctx->dec_stream_input));
    if (!ctx->dec_ctxs || !ctx->dec_stream_input)
        return AVERROR(ENOMEM);

    int dec_idx = 0;
    for (int i = 0; i < ctx->nb_inputs; i++) {
        AVFormatContext *fc = ctx->fmt_ctxs[i];
        for (unsigned int s = 0; s < fc->nb_streams; s++) {
            AVStream *st = fc->streams[s];
            const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);

            ctx->dec_stream_input[dec_idx] = i;

            if (codec && (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
                          st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)) {
                ctx->dec_ctxs[dec_idx] = avcodec_alloc_context3(codec);
                if (!ctx->dec_ctxs[dec_idx])
                    return AVERROR(ENOMEM);

                ret = avcodec_parameters_to_context(ctx->dec_ctxs[dec_idx], st->codecpar);
                if (ret < 0)
                    return ret;

                ctx->dec_ctxs[dec_idx]->pkt_timebase = st->time_base;

                ret = avcodec_open2(ctx->dec_ctxs[dec_idx], codec, NULL);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "super_concat: failed to open decoder for stream %d of '%s'\n",
                           s, seg->inputs[i].url);
                    return ret;
                }
            }
            dec_idx++;
        }
    }

    return 0;
}

/* ======================================================================
 * Build filtergraph for a segment
 * ====================================================================== */

/* Map a "[N:s]" style pad name to input_index and stream_index.
 * Also handles named pads from the filtergraph. */
static int sc_parse_stream_spec(const char *spec, int *input_idx, int *stream_idx,
                                char *type)
{
    /* format: [N:v] or [N:a] or [N:s:M] or just [0:v:0] */
    if (spec[0] == '[')
        spec++;
    int len = strlen(spec);
    if (len > 0 && spec[len - 1] == ']')
        len--;

    char buf[64];
    if (len >= (int)sizeof(buf))
        return -1;
    memcpy(buf, spec, len);
    buf[len] = '\0';

    /* try "N:type" or "N:type:M" */
    char *colon1 = strchr(buf, ':');
    if (!colon1)
        return -1;

    *colon1 = '\0';
    *input_idx = atoi(buf);
    char *rest = colon1 + 1;

    if (rest[0] == 'v' || rest[0] == 'V') {
        *type = 'v';
        *stream_idx = 0;
    } else if (rest[0] == 'a' || rest[0] == 'A') {
        *type = 'a';
        *stream_idx = 0;
    } else {
        *type = '?';
        *stream_idx = atoi(rest);
        return 0;
    }

    /* check for :N suffix */
    char *colon2 = strchr(rest, ':');
    if (colon2)
        *stream_idx = atoi(colon2 + 1);

    return 0;
}

/* Find the decoder index for a given input file and stream spec */
static int sc_find_decoder(SCSegmentCtx *ctx, int input_idx, char type, int stream_idx)
{
    int dec_offset = 0;
    for (int i = 0; i < input_idx && i < ctx->nb_inputs; i++)
        dec_offset += ctx->fmt_ctxs[i]->nb_streams;

    AVFormatContext *fc = ctx->fmt_ctxs[input_idx];
    int type_count = 0;
    for (unsigned int s = 0; s < fc->nb_streams; s++) {
        int match = 0;
        if (type == 'v' && fc->streams[s]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            match = 1;
        else if (type == 'a' && fc->streams[s]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            match = 1;

        if (match) {
            if (type_count == stream_idx)
                return dec_offset + s;
            type_count++;
        }
    }
    return -1;
}

static int sc_build_filtergraph(SCSegment *seg, SCSegmentCtx *ctx,
                                SCOutputStream *outputs, int nb_outputs)
{
    int ret;
    ctx->filter_graph = avfilter_graph_alloc();
    if (!ctx->filter_graph)
        return AVERROR(ENOMEM);

    if (seg->filter && seg->nb_maps > 0) {
        /* User-specified filtergraph with explicit maps */
        AVFilterInOut *filter_inputs = NULL;
        AVFilterInOut *filter_outputs = NULL;

        ret = avfilter_graph_parse2(ctx->filter_graph, seg->filter,
                                     &filter_inputs, &filter_outputs);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "super_concat: failed to parse filter '%s': %s\n",
                   seg->filter, av_err2str(ret));
            return ret;
        }

        /* Connect inputs: filter_inputs are the open input pads of the graph.
         * Their names correspond to stream specs like "0:v", "1:v", etc. */
        int nb_srcs = 0;
        for (AVFilterInOut *cur = filter_inputs; cur; cur = cur->next)
            nb_srcs++;

        ctx->nb_buffersrcs = nb_srcs;
        ctx->buffersrc_ctxs = av_calloc(nb_srcs, sizeof(*ctx->buffersrc_ctxs));
        if (!ctx->buffersrc_ctxs)
            return AVERROR(ENOMEM);

        int src_idx = 0;
        for (AVFilterInOut *cur = filter_inputs; cur; cur = cur->next, src_idx++) {
            int input_idx = 0, stream_idx = 0;
            char type = '?';
            sc_parse_stream_spec(cur->name, &input_idx, &stream_idx, &type);

            int dec_idx = sc_find_decoder(ctx, input_idx, type, stream_idx);
            if (dec_idx < 0 || !ctx->dec_ctxs[dec_idx]) {
                av_log(NULL, AV_LOG_ERROR, "super_concat: no decoder for filter input '%s'\n",
                       cur->name);
                avfilter_inout_free(&filter_inputs);
                avfilter_inout_free(&filter_outputs);
                return AVERROR(EINVAL);
            }

            AVCodecContext *dec = ctx->dec_ctxs[dec_idx];
            char args[512];
            char src_name[64];
            snprintf(src_name, sizeof(src_name), "src_%d", src_idx);

            if (dec->codec_type == AVMEDIA_TYPE_VIDEO) {
                snprintf(args, sizeof(args),
                         "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                         dec->width, dec->height, dec->pix_fmt,
                         dec->pkt_timebase.num, dec->pkt_timebase.den,
                         dec->sample_aspect_ratio.num,
                         FFMAX(dec->sample_aspect_ratio.den, 1));

                if (dec->framerate.num)
                    av_strlcatf(args, sizeof(args), ":frame_rate=%d/%d",
                                dec->framerate.num, dec->framerate.den);
            } else {
                char chl_buf[128];
                av_channel_layout_describe(&dec->ch_layout, chl_buf, sizeof(chl_buf));
                snprintf(args, sizeof(args),
                         "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
                         dec->pkt_timebase.num, dec->pkt_timebase.den,
                         dec->sample_rate,
                         av_get_sample_fmt_name(dec->sample_fmt),
                         chl_buf);
            }

            const AVFilter *buffersrc = avfilter_get_by_name(
                dec->codec_type == AVMEDIA_TYPE_VIDEO ? "buffer" : "abuffer");

            ret = avfilter_graph_create_filter(&ctx->buffersrc_ctxs[src_idx],
                                                buffersrc, src_name, args, NULL,
                                                ctx->filter_graph);
            if (ret < 0) {
                avfilter_inout_free(&filter_inputs);
                avfilter_inout_free(&filter_outputs);
                return ret;
            }

            ret = avfilter_link(ctx->buffersrc_ctxs[src_idx], 0,
                                cur->filter_ctx, cur->pad_idx);
            if (ret < 0) {
                avfilter_inout_free(&filter_inputs);
                avfilter_inout_free(&filter_outputs);
                return ret;
            }
        }

        /* Connect outputs: filter_outputs are the open output pads.
         * Match them to our output streams by name in maps. */
        int nb_sinks = 0;
        for (AVFilterInOut *cur = filter_outputs; cur; cur = cur->next)
            nb_sinks++;

        if (nb_sinks != nb_outputs) {
            av_log(NULL, AV_LOG_ERROR,
                   "super_concat: filter has %d outputs but expected %d\n",
                   nb_sinks, nb_outputs);
            avfilter_inout_free(&filter_inputs);
            avfilter_inout_free(&filter_outputs);
            return AVERROR(EINVAL);
        }

        ctx->nb_buffersinks = nb_sinks;
        ctx->buffersink_ctxs = av_calloc(nb_sinks, sizeof(*ctx->buffersink_ctxs));
        if (!ctx->buffersink_ctxs) {
            avfilter_inout_free(&filter_inputs);
            avfilter_inout_free(&filter_outputs);
            return AVERROR(ENOMEM);
        }

        /* Match filter outputs to our output streams.
         * filter_outputs come in the order of the maps array. */
        int sink_idx = 0;
        for (AVFilterInOut *cur = filter_outputs; cur; cur = cur->next, sink_idx++) {
            /* Determine which output stream this maps to */
            int out_idx = -1;
            for (int m = 0; m < seg->nb_maps; m++) {
                /* strip brackets for comparison */
                char map_name[64];
                const char *mn = seg->maps[m];
                if (mn[0] == '[') mn++;
                int ml = strlen(mn);
                if (ml > 0 && mn[ml - 1] == ']') ml--;
                snprintf(map_name, sizeof(map_name), "%.*s", ml, mn);

                if (!strcmp(cur->name, map_name)) {
                    out_idx = m;
                    break;
                }
            }

            if (out_idx < 0) {
                /* fallback: use index order */
                out_idx = sink_idx;
            }

            char sink_name[64];
            snprintf(sink_name, sizeof(sink_name), "sink_%d", out_idx);

            const AVFilter *buffersink = avfilter_get_by_name(
                outputs[out_idx].type == AVMEDIA_TYPE_VIDEO ? "buffersink" : "abuffersink");

            ret = avfilter_graph_create_filter(&ctx->buffersink_ctxs[out_idx],
                                                buffersink, sink_name, NULL, NULL,
                                                ctx->filter_graph);
            if (ret < 0) {
                avfilter_inout_free(&filter_inputs);
                avfilter_inout_free(&filter_outputs);
                return ret;
            }

            ret = avfilter_link(cur->filter_ctx, cur->pad_idx,
                                ctx->buffersink_ctxs[out_idx], 0);
            if (ret < 0) {
                avfilter_inout_free(&filter_inputs);
                avfilter_inout_free(&filter_outputs);
                return ret;
            }
        }

        avfilter_inout_free(&filter_inputs);
        avfilter_inout_free(&filter_outputs);

    } else {
        /* No filter specified: create passthrough (null/anull) for each output */
        ctx->nb_buffersrcs = nb_outputs;
        ctx->nb_buffersinks = nb_outputs;
        ctx->buffersrc_ctxs = av_calloc(nb_outputs, sizeof(*ctx->buffersrc_ctxs));
        ctx->buffersink_ctxs = av_calloc(nb_outputs, sizeof(*ctx->buffersink_ctxs));
        if (!ctx->buffersrc_ctxs || !ctx->buffersink_ctxs)
            return AVERROR(ENOMEM);

        for (int i = 0; i < nb_outputs; i++) {
            /* Find matching stream in first input */
            int dec_idx = -1;
            int type_count = 0;
            AVFormatContext *fc = ctx->fmt_ctxs[0];
            for (unsigned int s = 0; s < fc->nb_streams; s++) {
                if (fc->streams[s]->codecpar->codec_type == outputs[i].type) {
                    if (type_count == 0) {
                        /* find the absolute decoder index */
                        dec_idx = s; /* first input always starts at offset 0 */
                        break;
                    }
                    type_count++;
                }
            }

            if (dec_idx < 0 || !ctx->dec_ctxs[dec_idx]) {
                av_log(NULL, AV_LOG_ERROR,
                       "super_concat: no matching %s stream in input for output %d\n",
                       av_get_media_type_string(outputs[i].type), i);
                return AVERROR(EINVAL);
            }

            AVCodecContext *dec = ctx->dec_ctxs[dec_idx];
            char args[512];
            char name[64];

            if (dec->codec_type == AVMEDIA_TYPE_VIDEO) {
                snprintf(args, sizeof(args),
                         "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                         dec->width, dec->height, dec->pix_fmt,
                         dec->pkt_timebase.num, dec->pkt_timebase.den,
                         dec->sample_aspect_ratio.num,
                         FFMAX(dec->sample_aspect_ratio.den, 1));
                if (dec->framerate.num)
                    av_strlcatf(args, sizeof(args), ":frame_rate=%d/%d",
                                dec->framerate.num, dec->framerate.den);
            } else {
                char chl_buf[128];
                av_channel_layout_describe(&dec->ch_layout, chl_buf, sizeof(chl_buf));
                snprintf(args, sizeof(args),
                         "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
                         dec->pkt_timebase.num, dec->pkt_timebase.den,
                         dec->sample_rate,
                         av_get_sample_fmt_name(dec->sample_fmt),
                         chl_buf);
            }

            /* buffersrc */
            const AVFilter *buffersrc = avfilter_get_by_name(
                dec->codec_type == AVMEDIA_TYPE_VIDEO ? "buffer" : "abuffer");
            snprintf(name, sizeof(name), "src_%d", i);
            ret = avfilter_graph_create_filter(&ctx->buffersrc_ctxs[i],
                                                buffersrc, name, args, NULL,
                                                ctx->filter_graph);
            if (ret < 0)
                return ret;

            /* passthrough filter */
            const AVFilter *passthrough = avfilter_get_by_name(
                dec->codec_type == AVMEDIA_TYPE_VIDEO ? "null" : "anull");
            AVFilterContext *pass_ctx;
            snprintf(name, sizeof(name), "pass_%d", i);
            ret = avfilter_graph_create_filter(&pass_ctx, passthrough, name,
                                                NULL, NULL, ctx->filter_graph);
            if (ret < 0)
                return ret;

            /* buffersink */
            const AVFilter *buffersink = avfilter_get_by_name(
                dec->codec_type == AVMEDIA_TYPE_VIDEO ? "buffersink" : "abuffersink");
            snprintf(name, sizeof(name), "sink_%d", i);
            ret = avfilter_graph_create_filter(&ctx->buffersink_ctxs[i],
                                                buffersink, name, NULL, NULL,
                                                ctx->filter_graph);
            if (ret < 0)
                return ret;

            /* link: src -> pass -> sink */
            ret = avfilter_link(ctx->buffersrc_ctxs[i], 0, pass_ctx, 0);
            if (ret < 0)
                return ret;
            ret = avfilter_link(pass_ctx, 0, ctx->buffersink_ctxs[i], 0);
            if (ret < 0)
                return ret;
        }
    }

    ret = avfilter_graph_config(ctx->filter_graph, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "super_concat: failed to configure filtergraph: %s\n",
               av_err2str(ret));
        return ret;
    }

    return 0;
}

/* ======================================================================
 * Determine which decoder feeds which buffersrc
 * ====================================================================== */

typedef struct SrcMapping {
    int dec_idx;        /* index into ctx->dec_ctxs */
    int src_idx;        /* index into ctx->buffersrc_ctxs */
    int input_idx;      /* which input file */
    int stream_idx;     /* stream index within the input file */
} SrcMapping;

static int sc_build_src_mappings(SCSegment *seg, SCSegmentCtx *ctx,
                                  SCOutputStream *outputs, int nb_outputs,
                                  SrcMapping **out_mappings, int *out_nb)
{
    SrcMapping *mappings = NULL;
    int nb_mappings = 0;

    if (seg->filter && seg->nb_maps > 0) {
        /* Parse stream specs from filter to find which inputs feed which buffersrc.
         * We scan the filter string for patterns like [N:v], [N:a], [N:v:M] */
        /* The buffersrcs were created in order of the filter_inputs list.
         * We need to reconstruct that order. Re-parse the filter to get input pad names. */
        AVFilterInOut *filter_inputs = NULL;
        AVFilterInOut *filter_outputs = NULL;
        AVFilterGraph *tmp_graph = avfilter_graph_alloc();
        if (!tmp_graph)
            return AVERROR(ENOMEM);

        int ret = avfilter_graph_parse2(tmp_graph, seg->filter,
                                         &filter_inputs, &filter_outputs);
        if (ret < 0) {
            avfilter_graph_free(&tmp_graph);
            return ret;
        }

        int src_idx = 0;
        for (AVFilterInOut *cur = filter_inputs; cur; cur = cur->next, src_idx++) {
            int input_idx = 0, stream_idx_spec = 0;
            char type = '?';
            sc_parse_stream_spec(cur->name, &input_idx, &stream_idx_spec, &type);

            int dec_idx = sc_find_decoder(ctx, input_idx, type, stream_idx_spec);
            if (dec_idx < 0)
                continue;

            /* find stream_idx within input file */
            int dec_offset = 0;
            for (int i = 0; i < input_idx; i++)
                dec_offset += ctx->fmt_ctxs[i]->nb_streams;
            int local_stream_idx = dec_idx - dec_offset;

            SrcMapping *new_map = av_realloc_array(mappings, nb_mappings + 1,
                                                    sizeof(*mappings));
            if (!new_map) {
                av_free(mappings);
                avfilter_inout_free(&filter_inputs);
                avfilter_inout_free(&filter_outputs);
                avfilter_graph_free(&tmp_graph);
                return AVERROR(ENOMEM);
            }
            mappings = new_map;
            mappings[nb_mappings].dec_idx = dec_idx;
            mappings[nb_mappings].src_idx = src_idx;
            mappings[nb_mappings].input_idx = input_idx;
            mappings[nb_mappings].stream_idx = local_stream_idx;
            nb_mappings++;
        }

        avfilter_inout_free(&filter_inputs);
        avfilter_inout_free(&filter_outputs);
        avfilter_graph_free(&tmp_graph);

    } else {
        /* passthrough mode: one mapping per output */
        mappings = av_calloc(nb_outputs, sizeof(*mappings));
        if (!mappings)
            return AVERROR(ENOMEM);

        for (int i = 0; i < nb_outputs; i++) {
            /* Find matching stream in first input */
            AVFormatContext *fc = ctx->fmt_ctxs[0];
            for (unsigned int s = 0; s < fc->nb_streams; s++) {
                if (fc->streams[s]->codecpar->codec_type == outputs[i].type) {
                    mappings[nb_mappings].dec_idx = s;
                    mappings[nb_mappings].src_idx = i;
                    mappings[nb_mappings].input_idx = 0;
                    mappings[nb_mappings].stream_idx = s;
                    nb_mappings++;
                    break;
                }
            }
        }
    }

    *out_mappings = mappings;
    *out_nb = nb_mappings;
    return 0;
}

/* ======================================================================
 * Process one segment
 * ====================================================================== */

static int sc_process_segment(SCSegment *seg, SCSegmentCtx *ctx,
                               SCOutputStream *outputs, int nb_outputs,
                               AVFormatContext *ofmt_ctx, int segment_idx)
{
    int ret;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *filt_frame = av_frame_alloc();
    if (!pkt || !frame || !filt_frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* Build source mappings */
    SrcMapping *mappings = NULL;
    int nb_mappings = 0;
    ret = sc_build_src_mappings(seg, ctx, outputs, nb_outputs, &mappings, &nb_mappings);
    if (ret < 0)
        goto end;

    /* Processing loop */
    int all_eof = 0;
    while (!all_eof) {
        /* Find the input with smallest DTS that's not EOF */
        int best_input = -1;
        int64_t best_dts = INT64_MAX;
        for (int i = 0; i < ctx->nb_inputs; i++) {
            if (ctx->input_eof[i])
                continue;
            int64_t dts = ctx->input_last_dts[i];
            if (dts == AV_NOPTS_VALUE)
                dts = INT64_MIN;
            if (best_input < 0 || dts < best_dts) {
                best_dts = dts;
                best_input = i;
            }
        }

        if (best_input < 0) {
            all_eof = 1;
            break;
        }

        ret = av_read_frame(ctx->fmt_ctxs[best_input], pkt);
        if (ret == AVERROR_EOF) {
            ctx->input_eof[best_input] = 1;

            /* Check if all inputs are EOF */
            all_eof = 1;
            for (int i = 0; i < ctx->nb_inputs; i++) {
                if (!ctx->input_eof[i]) {
                    all_eof = 0;
                    break;
                }
            }
            continue;
        }
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "super_concat: error reading from input %d: %s\n",
                   best_input, av_err2str(ret));
            goto end;
        }

        /* Check duration limit */
        if (ctx->input_duration_limit[best_input] != AV_NOPTS_VALUE) {
            AVStream *st = ctx->fmt_ctxs[best_input]->streams[pkt->stream_index];
            int64_t pts_tb = av_rescale_q(pkt->pts, st->time_base, AV_TIME_BASE_Q);
            int64_t start = ctx->input_start_time[best_input];
            if (start == AV_NOPTS_VALUE) {
                /* use first packet PTS as start if no seek was done */
                start = pts_tb;
                ctx->input_start_time[best_input] = start;
            }
            if (pts_tb - start >= ctx->input_duration_limit[best_input]) {
                ctx->input_eof[best_input] = 1;
                av_packet_unref(pkt);
                all_eof = 1;
                for (int i = 0; i < ctx->nb_inputs; i++) {
                    if (!ctx->input_eof[i]) {
                        all_eof = 0;
                        break;
                    }
                }
                continue;
            }
        }

        /* Update last DTS for scheduling */
        AVStream *st = ctx->fmt_ctxs[best_input]->streams[pkt->stream_index];
        if (pkt->dts != AV_NOPTS_VALUE) {
            ctx->input_last_dts[best_input] =
                av_rescale_q(pkt->dts, st->time_base, AV_TIME_BASE_Q);
        }

        /* Find which buffersrc this packet feeds */
        int dec_offset = 0;
        for (int i = 0; i < best_input; i++)
            dec_offset += ctx->fmt_ctxs[i]->nb_streams;
        int abs_stream = dec_offset + pkt->stream_index;

        int mapping_idx = -1;
        for (int m = 0; m < nb_mappings; m++) {
            if (mappings[m].dec_idx == abs_stream) {
                mapping_idx = m;
                break;
            }
        }

        if (mapping_idx < 0 || !ctx->dec_ctxs[abs_stream]) {
            /* This stream isn't used by the filter */
            av_packet_unref(pkt);
            continue;
        }

        /* Decode */
        ret = avcodec_send_packet(ctx->dec_ctxs[abs_stream], pkt);
        av_packet_unref(pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            av_log(NULL, AV_LOG_ERROR, "super_concat: decode error: %s\n", av_err2str(ret));
            goto end;
        }

        while (1) {
            ret = avcodec_receive_frame(ctx->dec_ctxs[abs_stream], frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "super_concat: decode receive error: %s\n",
                       av_err2str(ret));
                goto end;
            }

            /* Push to buffersrc */
            ret = av_buffersrc_add_frame_flags(ctx->buffersrc_ctxs[mappings[mapping_idx].src_idx],
                                                frame, AV_BUFFERSRC_FLAG_PUSH);
            av_frame_unref(frame);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "super_concat: buffersrc error: %s\n",
                       av_err2str(ret));
                goto end;
            }

            /* Pull from all buffersinks */
            for (int s = 0; s < ctx->nb_buffersinks; s++) {
                if (!ctx->buffersink_ctxs[s])
                    continue;
                while (1) {
                    ret = av_buffersink_get_frame(ctx->buffersink_ctxs[s], filt_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                    if (ret < 0) {
                        av_log(NULL, AV_LOG_ERROR, "super_concat: buffersink error: %s\n",
                               av_err2str(ret));
                        goto end;
                    }

                    /* Adjust PTS for continuity */
                    SCOutputStream *out = &outputs[s];
                    int64_t orig_pts = filt_frame->pts;

                    /* Rescale from filter timebase to encoder timebase */
                    AVRational filter_tb = av_buffersink_get_time_base(ctx->buffersink_ctxs[s]);
                    int64_t pts_enc = av_rescale_q(orig_pts, filter_tb,
                                                    out->enc_ctx->time_base);

                    if (!out->got_first_pts) {
                        out->segment_base_pts = pts_enc;
                        out->got_first_pts = 1;
                    }

                    filt_frame->pts = (pts_enc - out->segment_base_pts) + out->ts_offset;
                    out->last_pts = filt_frame->pts;

                    /* Calculate duration */
                    if (out->type == AVMEDIA_TYPE_VIDEO) {
                        if (filt_frame->duration > 0) {
                            out->last_duration = av_rescale_q(filt_frame->duration,
                                                              filter_tb,
                                                              out->enc_ctx->time_base);
                        } else if (out->enc_ctx->framerate.num > 0) {
                            out->last_duration = av_rescale_q(1,
                                av_inv_q(out->enc_ctx->framerate),
                                out->enc_ctx->time_base);
                        } else {
                            out->last_duration = 1;
                        }
                    } else {
                        out->last_duration = av_rescale_q(filt_frame->nb_samples,
                            (AVRational){1, out->enc_ctx->sample_rate},
                            out->enc_ctx->time_base);
                        if (out->last_duration <= 0)
                            out->last_duration = 1;
                    }

                    filt_frame->pict_type = AV_PICTURE_TYPE_NONE;

                    /* Encode */
                    ret = avcodec_send_frame(out->enc_ctx, filt_frame);
                    av_frame_unref(filt_frame);
                    if (ret < 0) {
                        av_log(NULL, AV_LOG_ERROR, "super_concat: encode error: %s\n",
                               av_err2str(ret));
                        goto end;
                    }

                    while (1) {
                        AVPacket *enc_pkt = av_packet_alloc();
                        if (!enc_pkt) {
                            ret = AVERROR(ENOMEM);
                            goto end;
                        }
                        ret = avcodec_receive_packet(out->enc_ctx, enc_pkt);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                            av_packet_free(&enc_pkt);
                            break;
                        }
                        if (ret < 0) {
                            av_packet_free(&enc_pkt);
                            av_log(NULL, AV_LOG_ERROR, "super_concat: encode receive error: %s\n",
                                   av_err2str(ret));
                            goto end;
                        }

                        enc_pkt->stream_index = out->stream_index;
                        av_packet_rescale_ts(enc_pkt, out->enc_ctx->time_base,
                                             ofmt_ctx->streams[out->stream_index]->time_base);

                        ret = av_interleaved_write_frame(ofmt_ctx, enc_pkt);
                        av_packet_free(&enc_pkt);
                        if (ret < 0) {
                            av_log(NULL, AV_LOG_ERROR, "super_concat: mux error: %s\n",
                                   av_err2str(ret));
                            goto end;
                        }
                    }
                }
            }
        }
    }

    /* Flush decoders */
    for (int m = 0; m < nb_mappings; m++) {
        int dec_idx = mappings[m].dec_idx;
        if (!ctx->dec_ctxs[dec_idx])
            continue;

        avcodec_send_packet(ctx->dec_ctxs[dec_idx], NULL);
        while (1) {
            ret = avcodec_receive_frame(ctx->dec_ctxs[dec_idx], frame);
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                break;
            if (ret < 0)
                break;

            ret = av_buffersrc_add_frame_flags(ctx->buffersrc_ctxs[mappings[m].src_idx],
                                                frame, AV_BUFFERSRC_FLAG_PUSH);
            av_frame_unref(frame);
            if (ret < 0)
                break;
        }
    }

    /* Signal EOF to buffersrcs */
    for (int i = 0; i < ctx->nb_buffersrcs; i++) {
        if (ctx->buffersrc_ctxs[i]) {
            ret = av_buffersrc_add_frame(ctx->buffersrc_ctxs[i], NULL);
            if (ret < 0 && ret != AVERROR_EOF)
                av_log(NULL, AV_LOG_WARNING, "super_concat: buffersrc EOF signal failed: %s\n",
                       av_err2str(ret));
        }
    }

    /* Flush buffersinks */
    for (int s = 0; s < ctx->nb_buffersinks; s++) {
        if (!ctx->buffersink_ctxs[s])
            continue;
        while (1) {
            ret = av_buffersink_get_frame(ctx->buffersink_ctxs[s], filt_frame);
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                break;
            if (ret < 0)
                break;

            SCOutputStream *out = &outputs[s];
            int64_t orig_pts = filt_frame->pts;
            AVRational filter_tb = av_buffersink_get_time_base(ctx->buffersink_ctxs[s]);
            int64_t pts_enc = av_rescale_q(orig_pts, filter_tb, out->enc_ctx->time_base);

            if (!out->got_first_pts) {
                out->segment_base_pts = pts_enc;
                out->got_first_pts = 1;
            }

            filt_frame->pts = (pts_enc - out->segment_base_pts) + out->ts_offset;
            out->last_pts = filt_frame->pts;

            if (out->type == AVMEDIA_TYPE_VIDEO) {
                if (filt_frame->duration > 0)
                    out->last_duration = av_rescale_q(filt_frame->duration, filter_tb,
                                                      out->enc_ctx->time_base);
                else if (out->enc_ctx->framerate.num > 0)
                    out->last_duration = av_rescale_q(1, av_inv_q(out->enc_ctx->framerate),
                                                      out->enc_ctx->time_base);
                else
                    out->last_duration = 1;
            } else {
                out->last_duration = av_rescale_q(filt_frame->nb_samples,
                    (AVRational){1, out->enc_ctx->sample_rate},
                    out->enc_ctx->time_base);
                if (out->last_duration <= 0)
                    out->last_duration = 1;
            }

            filt_frame->pict_type = AV_PICTURE_TYPE_NONE;

            ret = avcodec_send_frame(out->enc_ctx, filt_frame);
            av_frame_unref(filt_frame);
            if (ret < 0)
                break;

            while (1) {
                AVPacket *enc_pkt = av_packet_alloc();
                if (!enc_pkt)
                    break;
                ret = avcodec_receive_packet(out->enc_ctx, enc_pkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    av_packet_free(&enc_pkt);
                    break;
                }
                if (ret < 0) {
                    av_packet_free(&enc_pkt);
                    break;
                }

                enc_pkt->stream_index = out->stream_index;
                av_packet_rescale_ts(enc_pkt, out->enc_ctx->time_base,
                                     ofmt_ctx->streams[out->stream_index]->time_base);

                ret = av_interleaved_write_frame(ofmt_ctx, enc_pkt);
                av_packet_free(&enc_pkt);
                if (ret < 0)
                    break;
            }
        }
    }

    /* Update timestamp offsets for next segment */
    for (int s = 0; s < nb_outputs; s++) {
        if (outputs[s].got_first_pts) {
            outputs[s].ts_offset = outputs[s].last_pts + outputs[s].last_duration;
        }
        outputs[s].got_first_pts = 0;
    }

    ret = 0;

end:
    av_free(mappings);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_frame_free(&filt_frame);
    return ret;
}

/* ======================================================================
 * Probe first segment to determine output layout
 * ====================================================================== */

static int sc_probe_output_layout(SCPlaylist *pl, SCOutputStream **out_streams,
                                   int *out_nb)
{
    SCSegment *seg = &pl->segments[0];
    SCSegmentCtx ctx = {0};
    int ret;

    ret = sc_open_inputs(seg, &ctx);
    if (ret < 0) {
        sc_segment_ctx_free(&ctx);
        return ret;
    }

    /* Determine outputs from maps or auto-detect */
    int nb_outputs = 0;
    SCOutputStream *outputs = NULL;

    if (seg->filter && seg->nb_maps > 0) {
        /* We need to build the filtergraph to query output formats */
        /* First, determine number of outputs from maps */
        nb_outputs = seg->nb_maps;
        outputs = av_calloc(nb_outputs, sizeof(*outputs));
        if (!outputs) {
            sc_segment_ctx_free(&ctx);
            return AVERROR(ENOMEM);
        }

        /* Determine type from map names */
        for (int i = 0; i < seg->nb_maps; i++) {
            const char *map = seg->maps[i];
            /* Try to detect type from name: if it contains 'v' -> video, 'a' -> audio */
            /* First check if it's a stream spec like [0:a] */
            int inp_idx, str_idx;
            char type;
            if (sc_parse_stream_spec(map, &inp_idx, &str_idx, &type) == 0 && type != '?') {
                outputs[i].type = (type == 'v') ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
            } else {
                /* Named pad - we need to build the graph to determine type.
                 * For now, try building a temporary graph. */
                outputs[i].type = AVMEDIA_TYPE_VIDEO; /* placeholder */
            }
        }

        /* Build temporary filtergraph to get actual output types and formats */
        AVFilterGraph *tmp_graph = avfilter_graph_alloc();
        if (!tmp_graph) {
            av_free(outputs);
            sc_segment_ctx_free(&ctx);
            return AVERROR(ENOMEM);
        }

        AVFilterInOut *fin = NULL, *fout = NULL;
        ret = avfilter_graph_parse2(tmp_graph, seg->filter, &fin, &fout);
        if (ret < 0) {
            avfilter_graph_free(&tmp_graph);
            av_free(outputs);
            sc_segment_ctx_free(&ctx);
            return ret;
        }

        /* Connect inputs with buffersrc */
        for (AVFilterInOut *cur = fin; cur; cur = cur->next) {
            int input_idx = 0, stream_idx_spec = 0;
            char type_ch = '?';
            sc_parse_stream_spec(cur->name, &input_idx, &stream_idx_spec, &type_ch);

            int dec_idx = sc_find_decoder(&ctx, input_idx, type_ch, stream_idx_spec);
            if (dec_idx < 0 || !ctx.dec_ctxs[dec_idx])
                continue;

            AVCodecContext *dec = ctx.dec_ctxs[dec_idx];
            char args[512];

            if (dec->codec_type == AVMEDIA_TYPE_VIDEO) {
                snprintf(args, sizeof(args),
                         "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                         dec->width, dec->height, dec->pix_fmt,
                         dec->pkt_timebase.num, dec->pkt_timebase.den,
                         dec->sample_aspect_ratio.num,
                         FFMAX(dec->sample_aspect_ratio.den, 1));
                if (dec->framerate.num)
                    av_strlcatf(args, sizeof(args), ":frame_rate=%d/%d",
                                dec->framerate.num, dec->framerate.den);
            } else {
                char chl_buf[128];
                av_channel_layout_describe(&dec->ch_layout, chl_buf, sizeof(chl_buf));
                snprintf(args, sizeof(args),
                         "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
                         dec->pkt_timebase.num, dec->pkt_timebase.den,
                         dec->sample_rate,
                         av_get_sample_fmt_name(dec->sample_fmt),
                         chl_buf);
            }

            const AVFilter *buffersrc = avfilter_get_by_name(
                dec->codec_type == AVMEDIA_TYPE_VIDEO ? "buffer" : "abuffer");
            AVFilterContext *src_ctx;
            ret = avfilter_graph_create_filter(&src_ctx, buffersrc, cur->name,
                                                args, NULL, tmp_graph);
            if (ret < 0)
                continue;
            avfilter_link(src_ctx, 0, cur->filter_ctx, cur->pad_idx);
        }

        /* Connect outputs with buffersink */
        int out_idx = 0;
        for (AVFilterInOut *cur = fout; cur; cur = cur->next, out_idx++) {
            /* Determine type from the pad */
            enum AVMediaType mtype = avfilter_pad_get_type(cur->filter_ctx->output_pads,
                                                            cur->pad_idx);
            if (out_idx < nb_outputs)
                outputs[out_idx].type = mtype;

            const AVFilter *sink = avfilter_get_by_name(
                mtype == AVMEDIA_TYPE_VIDEO ? "buffersink" : "abuffersink");
            AVFilterContext *sink_ctx;
            char name[32];
            snprintf(name, sizeof(name), "probe_sink_%d", out_idx);
            ret = avfilter_graph_create_filter(&sink_ctx, sink, name,
                                                NULL, NULL, tmp_graph);
            if (ret < 0)
                continue;
            avfilter_link(cur->filter_ctx, cur->pad_idx, sink_ctx, 0);
        }

        ret = avfilter_graph_config(tmp_graph, NULL);
        if (ret >= 0) {
            /* Query output formats from configured sinks */
            for (int i = 0; i < nb_outputs && i < out_idx; i++) {
                /* Find the sink by name */
                char name[32];
                snprintf(name, sizeof(name), "probe_sink_%d", i);
                AVFilterContext *sink = avfilter_graph_get_filter(tmp_graph, name);
                if (!sink)
                    continue;

                if (outputs[i].type == AVMEDIA_TYPE_VIDEO) {
                    outputs[i].width = av_buffersink_get_w(sink);
                    outputs[i].height = av_buffersink_get_h(sink);
                    outputs[i].pix_fmt = av_buffersink_get_format(sink);
                    outputs[i].frame_rate = av_buffersink_get_frame_rate(sink);
                    outputs[i].sample_aspect_ratio = av_buffersink_get_sample_aspect_ratio(sink);
                } else if (outputs[i].type == AVMEDIA_TYPE_AUDIO) {
                    outputs[i].sample_rate = av_buffersink_get_sample_rate(sink);
                    outputs[i].sample_fmt = av_buffersink_get_format(sink);
                    av_buffersink_get_ch_layout(sink, &outputs[i].ch_layout);
                }
            }
        }

        avfilter_inout_free(&fin);
        avfilter_inout_free(&fout);
        avfilter_graph_free(&tmp_graph);

    } else {
        /* No filter: auto-detect from first input's streams */
        AVFormatContext *fc = ctx.fmt_ctxs[0];
        for (unsigned int s = 0; s < fc->nb_streams; s++) {
            AVCodecParameters *par = fc->streams[s]->codecpar;
            if (par->codec_type != AVMEDIA_TYPE_VIDEO &&
                par->codec_type != AVMEDIA_TYPE_AUDIO)
                continue;

            SCOutputStream *new_out = av_realloc_array(outputs, nb_outputs + 1,
                                                        sizeof(*outputs));
            if (!new_out) {
                av_free(outputs);
                sc_segment_ctx_free(&ctx);
                return AVERROR(ENOMEM);
            }
            outputs = new_out;
            memset(&outputs[nb_outputs], 0, sizeof(outputs[nb_outputs]));

            outputs[nb_outputs].type = par->codec_type;
            if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
                outputs[nb_outputs].width = par->width;
                outputs[nb_outputs].height = par->height;
                outputs[nb_outputs].pix_fmt = par->format;
                outputs[nb_outputs].frame_rate = fc->streams[s]->avg_frame_rate;
                outputs[nb_outputs].sample_aspect_ratio = par->sample_aspect_ratio;
            } else {
                outputs[nb_outputs].sample_rate = par->sample_rate;
                outputs[nb_outputs].sample_fmt = par->format;
                av_channel_layout_copy(&outputs[nb_outputs].ch_layout, &par->ch_layout);
            }
            nb_outputs++;
        }
    }

    sc_segment_ctx_free(&ctx);

    if (nb_outputs == 0) {
        av_log(NULL, AV_LOG_ERROR, "super_concat: no output streams detected\n");
        av_free(outputs);
        return AVERROR(EINVAL);
    }

    *out_streams = outputs;
    *out_nb = nb_outputs;
    return 0;
}

/* ======================================================================
 * Create output file with encoders
 * ====================================================================== */

static int sc_create_output(const char *output_url, const char *format,
                             SCOutputStream *outputs, int nb_outputs,
                             SCOutputOpts *opts, AVFormatContext **out_fmt)
{
    int ret;

    ret = avformat_alloc_output_context2(out_fmt, NULL, format, output_url);
    if (ret < 0 || !*out_fmt) {
        av_log(NULL, AV_LOG_ERROR, "super_concat: failed to create output context for '%s'\n",
               output_url);
        return ret < 0 ? ret : AVERROR(ENOMEM);
    }

    for (int i = 0; i < nb_outputs; i++) {
        const AVCodec *codec = NULL;
        const char *codec_name = NULL;

        if (outputs[i].type == AVMEDIA_TYPE_VIDEO)
            codec_name = opts->vcodec;
        else if (outputs[i].type == AVMEDIA_TYPE_AUDIO)
            codec_name = opts->acodec;

        if (codec_name) {
            codec = avcodec_find_encoder_by_name(codec_name);
            if (!codec) {
                av_log(NULL, AV_LOG_ERROR, "super_concat: encoder '%s' not found\n",
                       codec_name);
                return AVERROR_ENCODER_NOT_FOUND;
            }
        } else {
            /* Use default codec for the format */
            enum AVCodecID codec_id = av_guess_codec((*out_fmt)->oformat, NULL, output_url,
                                                      NULL, outputs[i].type);
            codec = avcodec_find_encoder(codec_id);
            if (!codec) {
                av_log(NULL, AV_LOG_ERROR, "super_concat: no default encoder for %s\n",
                       av_get_media_type_string(outputs[i].type));
                return AVERROR_ENCODER_NOT_FOUND;
            }
        }

        AVStream *st = avformat_new_stream(*out_fmt, NULL);
        if (!st)
            return AVERROR(ENOMEM);

        outputs[i].stream_index = st->index;

        AVCodecContext *enc = avcodec_alloc_context3(codec);
        if (!enc)
            return AVERROR(ENOMEM);

        if (outputs[i].type == AVMEDIA_TYPE_VIDEO) {
            enc->width = outputs[i].width;
            enc->height = outputs[i].height;
            enc->pix_fmt = outputs[i].pix_fmt;
            enc->sample_aspect_ratio = outputs[i].sample_aspect_ratio;

            /* Check if the pixel format is supported */
            {
                const void *pix_fmt_list = NULL;
                int nb_pix_fmts = 0;
                if (avcodec_get_supported_config(enc, codec, AV_CODEC_CONFIG_PIX_FORMAT,
                                                  0, &pix_fmt_list, &nb_pix_fmts) >= 0 &&
                    pix_fmt_list && nb_pix_fmts > 0) {
                    const enum AVPixelFormat *pfmts = pix_fmt_list;
                    int supported = 0;
                    for (int pi = 0; pi < nb_pix_fmts; pi++) {
                        if (pfmts[pi] == enc->pix_fmt) {
                            supported = 1;
                            break;
                        }
                    }
                    if (!supported)
                        enc->pix_fmt = pfmts[0];
                }
            }

            if (outputs[i].frame_rate.num > 0) {
                enc->framerate = outputs[i].frame_rate;
                enc->time_base = av_inv_q(outputs[i].frame_rate);
            } else {
                enc->time_base = (AVRational){1, 25};
                enc->framerate = (AVRational){25, 1};
            }

            if ((*out_fmt)->oformat->flags & AVFMT_GLOBALHEADER)
                enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            /* Apply video options */
            AVDictionary *vopts_copy = NULL;
            av_dict_copy(&vopts_copy, opts->vopts, 0);

            /* Handle pix_fmt override */
            AVDictionaryEntry *pf = av_dict_get(vopts_copy, "pix_fmt", NULL, 0);
            if (pf) {
                enum AVPixelFormat pfmt = av_get_pix_fmt(pf->value);
                if (pfmt != AV_PIX_FMT_NONE)
                    enc->pix_fmt = pfmt;
                av_dict_set(&vopts_copy, "pix_fmt", NULL, 0);
            }

            ret = avcodec_open2(enc, codec, &vopts_copy);
            av_dict_free(&vopts_copy);
        } else {
            enc->sample_rate = outputs[i].sample_rate;
            enc->sample_fmt = outputs[i].sample_fmt;
            av_channel_layout_copy(&enc->ch_layout, &outputs[i].ch_layout);

            /* Check if sample format is supported */
            {
                const void *sfmt_list = NULL;
                int nb_sfmts = 0;
                if (avcodec_get_supported_config(enc, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                                  0, &sfmt_list, &nb_sfmts) >= 0 &&
                    sfmt_list && nb_sfmts > 0) {
                    const enum AVSampleFormat *sfmts = sfmt_list;
                    int supported = 0;
                    for (int si = 0; si < nb_sfmts; si++) {
                        if (sfmts[si] == enc->sample_fmt) {
                            supported = 1;
                            break;
                        }
                    }
                    if (!supported)
                        enc->sample_fmt = sfmts[0];
                }
            }

            enc->time_base = (AVRational){1, enc->sample_rate};

            if ((*out_fmt)->oformat->flags & AVFMT_GLOBALHEADER)
                enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            AVDictionary *aopts_copy = NULL;
            av_dict_copy(&aopts_copy, opts->aopts, 0);
            ret = avcodec_open2(enc, codec, &aopts_copy);
            av_dict_free(&aopts_copy);
        }

        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "super_concat: failed to open encoder: %s\n",
                   av_err2str(ret));
            avcodec_free_context(&enc);
            return ret;
        }

        ret = avcodec_parameters_from_context(st->codecpar, enc);
        if (ret < 0) {
            avcodec_free_context(&enc);
            return ret;
        }

        st->time_base = enc->time_base;
        outputs[i].enc_ctx = enc;
    }

    /* Open output file */
    if (!((*out_fmt)->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&(*out_fmt)->pb, output_url, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "super_concat: failed to open output '%s': %s\n",
                   output_url, av_err2str(ret));
            return ret;
        }
    }

    ret = avformat_write_header(*out_fmt, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "super_concat: failed to write header: %s\n",
               av_err2str(ret));
        return ret;
    }

    return 0;
}

/* ======================================================================
 * Read entire file into a string
 * ====================================================================== */

static char *sc_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        av_log(NULL, AV_LOG_ERROR, "super_concat: cannot open '%s'\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 10 * 1024 * 1024) {
        av_log(NULL, AV_LOG_ERROR, "super_concat: invalid file size for '%s'\n", path);
        fclose(f);
        return NULL;
    }

    char *buf = av_malloc(size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t nread = fread(buf, 1, size, f);
    fclose(f);
    buf[nread] = '\0';

    return buf;
}

/* ======================================================================
 * Main entry point
 * ====================================================================== */

int super_concat_main(const char *playlist_file, const char *output_url,
                      int argc, char **argv)
{
    int ret;
    SCPlaylist *playlist = NULL;
    AVFormatContext *ofmt_ctx = NULL;
    SCOutputStream *outputs = NULL;
    int nb_outputs = 0;
    SCOutputOpts out_opts;

    av_log(NULL, AV_LOG_INFO, "super_concat: processing playlist '%s' -> '%s'\n",
           playlist_file, output_url);

    /* Parse output options */
    sc_parse_output_opts(argc, argv, &out_opts);

    /* Read and parse playlist */
    char *json_text = sc_read_file(playlist_file);
    if (!json_text)
        return AVERROR(EIO);

    playlist = sc_playlist_parse(json_text);
    av_free(json_text);
    if (!playlist)
        return AVERROR(EINVAL);

    av_log(NULL, AV_LOG_INFO, "super_concat: playlist has %d segment(s)\n",
           playlist->nb_segments);

    /* Probe first segment for output layout */
    ret = sc_probe_output_layout(playlist, &outputs, &nb_outputs);
    if (ret < 0)
        goto cleanup;

    av_log(NULL, AV_LOG_INFO, "super_concat: output layout: %d stream(s)\n", nb_outputs);
    for (int i = 0; i < nb_outputs; i++) {
        if (outputs[i].type == AVMEDIA_TYPE_VIDEO) {
            av_log(NULL, AV_LOG_INFO, "  stream %d: video %dx%d %s\n", i,
                   outputs[i].width, outputs[i].height,
                   av_get_pix_fmt_name(outputs[i].pix_fmt));
        } else {
            char chl[64];
            av_channel_layout_describe(&outputs[i].ch_layout, chl, sizeof(chl));
            av_log(NULL, AV_LOG_INFO, "  stream %d: audio %d Hz %s %s\n", i,
                   outputs[i].sample_rate,
                   av_get_sample_fmt_name(outputs[i].sample_fmt), chl);
        }
    }

    /* Create output with encoders */
    ret = sc_create_output(output_url, out_opts.format, outputs, nb_outputs,
                            &out_opts, &ofmt_ctx);
    if (ret < 0)
        goto cleanup;

    /* Process each segment */
    for (int i = 0; i < playlist->nb_segments; i++) {
        av_log(NULL, AV_LOG_INFO, "super_concat: processing segment %d/%d\n",
               i + 1, playlist->nb_segments);

        SCSegmentCtx seg_ctx = {0};
        ret = sc_open_inputs(&playlist->segments[i], &seg_ctx);
        if (ret < 0) {
            sc_segment_ctx_free(&seg_ctx);
            goto cleanup;
        }

        ret = sc_build_filtergraph(&playlist->segments[i], &seg_ctx,
                                    outputs, nb_outputs);
        if (ret < 0) {
            sc_segment_ctx_free(&seg_ctx);
            goto cleanup;
        }

        ret = sc_process_segment(&playlist->segments[i], &seg_ctx,
                                  outputs, nb_outputs, ofmt_ctx, i);
        sc_segment_ctx_free(&seg_ctx);
        if (ret < 0)
            goto cleanup;

        av_log(NULL, AV_LOG_INFO, "super_concat: segment %d complete\n", i + 1);
    }

    /* Flush encoders */
    for (int i = 0; i < nb_outputs; i++) {
        avcodec_send_frame(outputs[i].enc_ctx, NULL);
        while (1) {
            AVPacket *pkt = av_packet_alloc();
            if (!pkt)
                break;
            ret = avcodec_receive_packet(outputs[i].enc_ctx, pkt);
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
                av_packet_free(&pkt);
                break;
            }
            if (ret < 0) {
                av_packet_free(&pkt);
                break;
            }
            pkt->stream_index = outputs[i].stream_index;
            av_packet_rescale_ts(pkt, outputs[i].enc_ctx->time_base,
                                 ofmt_ctx->streams[outputs[i].stream_index]->time_base);
            av_interleaved_write_frame(ofmt_ctx, pkt);
            av_packet_free(&pkt);
        }
    }

    /* Write trailer */
    ret = av_write_trailer(ofmt_ctx);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "super_concat: failed to write trailer: %s\n",
               av_err2str(ret));
    } else {
        av_log(NULL, AV_LOG_INFO, "super_concat: output written successfully\n");
        ret = 0;
    }

cleanup:
    /* Free encoders */
    if (outputs) {
        for (int i = 0; i < nb_outputs; i++) {
            avcodec_free_context(&outputs[i].enc_ctx);
            if (outputs[i].type == AVMEDIA_TYPE_AUDIO)
                av_channel_layout_uninit(&outputs[i].ch_layout);
        }
        av_free(outputs);
    }

    /* Close output */
    if (ofmt_ctx) {
        if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&ofmt_ctx->pb);
        avformat_free_context(ofmt_ctx);
    }

    sc_playlist_free(playlist);
    sc_output_opts_free(&out_opts);

    return ret;
}
