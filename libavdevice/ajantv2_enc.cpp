/*
 * AJA NTV2 output device
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

/*
 * Video and SD PAL teletext out of an AJA card, as a counterpart to the
 * DeckLink output.
 *
 * All the teletext behaviour is in libavdevice/vbi_teletext.c and is shared
 * with the DeckLink output, so the two cards put the same waveform and the same
 * page structure on air. This file only has to put the rendered line somewhere,
 * and on AJA that is easier than on DeckLink: with the VANC mode set to TALLER
 * the vertical blanking lines are simply rows of the frame buffer, so line 21
 * is written by writing into the buffer at the right row rather than through a
 * separate ancillary interface.
 *
 * What is here: 625i50 video in v210 or UYVY, and teletext on lines 21 and 334.
 *
 * What is not here yet: audio, the asynchronous output buffer and pre-render
 * machinery the DeckLink output has, genlock selection, and HD modes with
 * OP-47 teletext in VANC. The frame path is deliberately the simple one --
 * AutoCirculate with a ring, one transfer per frame -- so that the first thing
 * proven on a card is the picture and the captions.
 */

#include <memory>
#include <string>
#include <vector>

#include "ntv2card.h"
#include "ntv2devicescanner.h"
#include "ntv2devicefeatures.h"
#include "ntv2formatdescriptor.h"
#include "ntv2utils.h"
#include "ajabase/system/systemtime.h"

extern "C" {
#include "libavformat/avformat.h"
#include "libavformat/internal.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/avstring.h"
#include "libavutil/frame.h"

#include "ajantv2_common_c.h"
#include "ajantv2_enc.h"
#include "vbi_teletext.h"
}

/* SD teletext lives on line 21 of field 1 and the line 313 further on in
 * field 2, per OP-42 and Australian practice. */
#define AJA_SD_LINE_FIELD1  21
#define AJA_SD_LINE_FIELD2  334

struct ajantv2_ctx {
    CNTV2Card           device;
    NTV2Channel         channel;              /* framestore */
    std::vector<NTV2Channel> outputs;          /* SDI connectors fed from it */
    NTV2VideoFormat     video_format;
    NTV2PixelFormat     pixel_format;
    NTV2VANCMode        vanc_mode;
    NTV2FormatDescriptor fd;
    ULWord              frame_size;
    ULWord              ring_first;
    ULWord              ring_last;

    NTV2Buffer          host_buffer;
    AUTOCIRCULATE_TRANSFER xfer;

    int64_t             last_pts;
    int                 frames_out;

    /* Teletext, all of it device-independent. */
    FFVbiTeletextContext tt;
    AVStream            *teletext_st;
    int                  teletext_enabled;
    ULWord               tt_row_f1;
    ULWord               tt_row_f2;
};

/* Pick the frame buffer format from the codec, the way the DeckLink output does.
 * The pixel format of an encoded stream is not reliable at write_header time --
 * a v210 stream has been seen reporting yuv422p there -- so the codec is what
 * decides, and v210 is the one that can carry teletext.
 */
static bool aja_frame_format(AVFormatContext *avctx, AVCodecParameters *c,
                            NTV2PixelFormat *out)
{
    if (c->codec_id == AV_CODEC_ID_WRAPPED_AVFRAME) {
        if (c->format != AV_PIX_FMT_UYVY422) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported pixel format!"
                   " Only AV_PIX_FMT_UYVY422 is supported for wrapped frames.\n");
            return false;
        }
        *out = NTV2_FBF_8BIT_YCBCR;
        return true;
    }
    if (c->codec_id == AV_CODEC_ID_V210) {
        *out = NTV2_FBF_10BIT_YCBCR;
        return true;
    }
    av_log(avctx, AV_LOG_ERROR, "Unsupported codec type!"
           " Only V210 and wrapped frames with AV_PIX_FMT_UYVY422 are supported.\n");
    return false;
}

/* Find a video format by the name the SDK gives it -- "1080i50", "1080p25",
 * "625i50". Matched against NTV2VideoFormatToString over every format rather
 * than a table here, so the accepted spellings are the SDK's own and cannot
 * drift from it. */
static NTV2VideoFormat aja_format_by_name(const char *name)
{
    for (int f = NTV2_FORMAT_FIRST_HIGH_DEF_FORMAT; f < NTV2_FORMAT_END_STANDARD_DEF_FORMATS; f++) {
        const NTV2VideoFormat vf = NTV2VideoFormat(f);
        const std::string s = ::NTV2VideoFormatToString(vf);
        if (!s.empty() && !av_strcasecmp(s.c_str(), name))
            return vf;
    }
    return NTV2_FORMAT_UNKNOWN;
}

static void aja_log_formats(AVFormatContext *avctx)
{
    std::string line;
    av_log(avctx, AV_LOG_INFO, "Video formats this build knows:\n");
    for (int f = NTV2_FORMAT_FIRST_HIGH_DEF_FORMAT; f < NTV2_FORMAT_END_STANDARD_DEF_FORMATS; f++) {
        const std::string s = ::NTV2VideoFormatToString(NTV2VideoFormat(f));
        if (s.empty() || s.find('?') != std::string::npos)
            continue;
        line += "  " + s;
        if (line.size() > 64) {
            av_log(avctx, AV_LOG_INFO, "%s\n", line.c_str());
            line.clear();
        }
    }
    if (!line.empty())
        av_log(avctx, AV_LOG_INFO, "%s\n", line.c_str());
}

/* Map the stream's raster and rate onto an NTV2 video format.
 *
 * The SDK will do the matching, so this does not carry a table: the rate comes
 * from the stream's frame rate as a fraction, and interlaced-versus-progressive
 * from the stream's field order. An interlaced stream carries fields at twice
 * the frame rate in ffmpeg's reckoning, which is not what
 * GetFirstMatchingVideoFormat wants, so the rate handed to it is the frame rate.
 */
static bool aja_video_format(AVFormatContext *avctx, struct ajantv2_cctx *cctx,
                            AVStream *st, NTV2VideoFormat *out)
{
    AVCodecParameters *par = st->codecpar;
    AVRational fr = st->avg_frame_rate;
    const bool interlaced = par->field_order != AV_FIELD_PROGRESSIVE &&
                            par->field_order != AV_FIELD_UNKNOWN;
    NTV2FrameRate rate;

    /* An explicit name wins. It has to be available, because a raster and a
     * frame rate do not always identify a format: 1080i50 and 1080p25 are both
     * 1920x1080 at 25, and which one goes down the wire is a different SDI
     * standard. ffmpeg often reaches a muxer with the field order unset, so
     * there is nothing to tell them apart by. */
    if (cctx->format_code && *cctx->format_code) {
        *out = aja_format_by_name(cctx->format_code);
        if (*out == NTV2_FORMAT_UNKNOWN) {
            av_log(avctx, AV_LOG_ERROR, "Unknown format_code '%s'\n", cctx->format_code);
            aja_log_formats(avctx);
            return false;
        }
        av_log(avctx, AV_LOG_INFO, "Video format %s (from format_code)\n",
               ::NTV2VideoFormatToString(*out).c_str());
        return true;
    }

    if (!fr.num || !fr.den)
        fr = st->r_frame_rate;
    if (!fr.num || !fr.den) {
        av_log(avctx, AV_LOG_ERROR, "Cannot determine the stream frame rate\n");
        return false;
    }

    rate = ::GetNTV2FrameRateFromNumeratorDenominator(fr.num, fr.den);
    if (rate == NTV2_FRAMERATE_UNKNOWN) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported frame rate %d/%d\n", fr.num, fr.den);
        return false;
    }

    *out = ::GetFirstMatchingVideoFormat(rate, (UWord)par->height, (UWord)par->width,
                                         interlaced, false, false);
    if (*out == NTV2_FORMAT_UNKNOWN && interlaced) {
        /* Some 1080 sources are segmented-frame rather than true interlace, and
         * the two are different formats to the SDK even though the raster and
         * rate match. Try PSF before giving up. */
        *out = ::GetFirstMatchingVideoFormat(rate, (UWord)par->height,
                                            (UWord)par->width, true, false, true);
    }
    if (*out == NTV2_FORMAT_UNKNOWN) {
        av_log(avctx, AV_LOG_ERROR,
               "No NTV2 video format for %dx%d %s at %d/%d\n",
               par->width, par->height, interlaced ? "interlaced" : "progressive",
               fr.num, fr.den);
        return false;
    }
    av_log(avctx, AV_LOG_INFO, "%dx%d %s %d/%d -> %s\n",
           par->width, par->height, interlaced ? "interlaced" : "progressive",
           fr.num, fr.den, ::NTV2VideoFormatToString(*out).c_str());
    if (par->field_order == AV_FIELD_UNKNOWN && par->height >= 1080)
        av_log(avctx, AV_LOG_WARNING,
               "The stream does not say whether it is interlaced, so %s was "
               "guessed. Pass -format_code to be certain: 1080i50 and 1080p25 "
               "share this raster and rate.\n",
               ::NTV2VideoFormatToString(*out).c_str());
    return true;
}

static int aja_setup_video(AVFormatContext *avctx, struct ajantv2_ctx *ctx,
                           struct ajantv2_cctx *cctx, AVStream *st)
{
    AVCodecParameters *par = st->codecpar;

    if (!aja_video_format(avctx, cctx, st, &ctx->video_format))
        return AVERROR(EINVAL);
    if (!aja_frame_format(avctx, par, &ctx->pixel_format))
        return AVERROR(EINVAL);

    if (!ctx->device.features().CanDoVideoFormat(ctx->video_format)) {
        av_log(avctx, AV_LOG_ERROR, "Device cannot do %s\n",
               ::NTV2VideoFormatToString(ctx->video_format).c_str());
        return AVERROR(EINVAL);
    }

    /* The VBI renderer writes V210, so teletext needs the 10-bit frame buffer
     * format. Rendering it into a UYVY row would put noise on line 21 rather
     * than fail, so refuse the combination instead. */
    if (ctx->teletext_enabled && ctx->pixel_format != NTV2_FBF_10BIT_YCBCR) {
        av_log(avctx, AV_LOG_ERROR,
               "Teletext output needs -c:v v210; uyvy422 cannot carry the VBI "
               "waveform\n");
        return AVERROR(EINVAL);
    }

    /* TALLER puts the vertical blanking lines at the top of the frame buffer,
     * which is what makes writing line 21 a matter of writing into the buffer.
     * Only asked for when teletext is actually wanted, since it changes the
     * buffer geometry. */
    if (ctx->teletext_enabled && !NTV2_IS_625_FORMAT(ctx->video_format)) {
        av_log(avctx, AV_LOG_WARNING,
               "Teletext output only applies to 625-line SD; ignoring the "
               "subtitle stream for %s\n",
               ::NTV2VideoFormatToString(ctx->video_format).c_str());
        ctx->teletext_enabled = 0;
    }
    ctx->vanc_mode = ctx->teletext_enabled ? NTV2_VANCMODE_TALLER
                                           : NTV2_VANCMODE_OFF;

    ctx->device.SetVideoFormat(ctx->video_format, false, false, ctx->channel);
    ctx->device.SetFrameBufferFormat(ctx->channel, ctx->pixel_format);
    ctx->device.SetVANCMode(ctx->vanc_mode, ctx->channel);
    ctx->device.SetMode(ctx->channel, NTV2_MODE_DISPLAY);
    ctx->device.EnableChannel(ctx->channel);

    /* 8-bit VANC is shifted by the hardware unless told otherwise; 10-bit is
     * not. Match what the VBI renderer writes. */
    if (NTV2_IS_VANCMODE_ON(ctx->vanc_mode))
        ctx->device.SetVANCShiftMode(ctx->channel,
            ctx->pixel_format == NTV2_FBF_8BIT_YCBCR
                ? NTV2_VANCDATA_8BITSHIFT_ENABLE : NTV2_VANCDATA_8BITSHIFT_DISABLE);

    ctx->fd = NTV2FormatDescriptor(ctx->video_format, ctx->pixel_format,
                                   ctx->vanc_mode);
    ctx->frame_size = ::GetVideoWriteSize(ctx->video_format, ctx->pixel_format,
                                         ctx->vanc_mode);

    if (ctx->teletext_enabled) {
        if (!ctx->fd.GetLineOffsetFromSMPTELine(AJA_SD_LINE_FIELD1, ctx->tt_row_f1) ||
            !ctx->fd.GetLineOffsetFromSMPTELine(AJA_SD_LINE_FIELD2, ctx->tt_row_f2)) {
            av_log(avctx, AV_LOG_ERROR,
                   "Cannot locate VBI lines %d/%d in the frame buffer\n",
                   AJA_SD_LINE_FIELD1, AJA_SD_LINE_FIELD2);
            return AVERROR(EINVAL);
        }
        av_log(avctx, AV_LOG_INFO,
               "Teletext on SMPTE lines %d and %d (buffer rows %u and %u)\n",
               AJA_SD_LINE_FIELD1, AJA_SD_LINE_FIELD2,
               ctx->tt_row_f1, ctx->tt_row_f2);
    }

    /* Route the framestore to every requested SDI connector. One framestore can
     * feed several, which is how a card with two outputs mirrors the same
     * picture, so this is a list rather than a single choice. */
    {
        const NTV2OutputCrosspointID fsOut(::GetFrameBufferOutputXptFromChannel(ctx->channel));
        const NTV2Standard std(::GetNTV2StandardFromVideoFormat(ctx->video_format));
        const UWord num_outputs(ctx->device.features().GetNumVideoOutputs());
        std::string listed;

        for (size_t n = 0; n < ctx->outputs.size(); n++) {
            const NTV2Channel out(ctx->outputs[n]);
            if (num_outputs && UWord(out) >= num_outputs) {
                av_log(avctx, AV_LOG_ERROR,
                       "SDI output %d requested but this device has %u\n",
                       int(out) + 1, num_outputs);
                return AVERROR(EINVAL);
            }
            ctx->device.Connect(::GetSDIOutputInputXpt(out), fsOut);
            ctx->device.SetSDITransmitEnable(out, true);
            ctx->device.SetSDIOutputStandard(out, std);
            listed += (n ? ", " : "");
            listed += std::to_string(int(out) + 1);
        }
        av_log(avctx, AV_LOG_INFO, "Framestore %d -> SDI output%s %s\n",
               int(ctx->channel) + 1, ctx->outputs.size() > 1 ? "s" : "",
               listed.c_str());
    }

    if (!ctx->host_buffer.Allocate(ctx->frame_size, true)) {
        av_log(avctx, AV_LOG_ERROR, "Cannot allocate a %u-byte frame buffer\n",
               ctx->frame_size);
        return AVERROR(ENOMEM);
    }

    const ULWord ring = cctx->ring_frames > 1 ? (ULWord)cctx->ring_frames : 8;
    ctx->device.AutoCirculateStop(ctx->channel);
    if (!ctx->device.AutoCirculateInitForOutput(ctx->channel, ring)) {
        av_log(avctx, AV_LOG_ERROR, "AutoCirculateInitForOutput failed\n");
        return AVERROR(EIO);
    }
    if (!ctx->device.AutoCirculateStart(ctx->channel)) {
        av_log(avctx, AV_LOG_ERROR, "AutoCirculateStart failed\n");
        return AVERROR(EIO);
    }
    av_log(avctx, AV_LOG_INFO, "AJA output: %s %s, %u-frame ring, buffer %u bytes\n",
           ::NTV2VideoFormatToString(ctx->video_format).c_str(),
           ::NTV2FrameBufferFormatToString(ctx->pixel_format).c_str(),
           ring, ctx->frame_size);
    return 0;
}

/* Render whatever the teletext generator says goes out this frame into the two
 * VBI rows of the frame buffer. */
static void aja_insert_teletext(AVFormatContext *avctx, struct ajantv2_ctx *ctx,
                                int stored_new)
{
    FFVbiTeletextFrame out;
    const ULWord rows[2] = {ctx->tt_row_f1, ctx->tt_row_f2};
    int f;

    ff_vbi_teletext_frame(&ctx->tt, stored_new, ctx->last_pts, &out, avctx);

    for (f = 0; f < 2; f++) {
        void *row;
        if (!out.send[f])
            continue;
        row = ctx->fd.GetWriteableRowAddress(ctx->host_buffer.GetHostPointer(),
                                             rows[f]);
        if (!row)
            continue;
        ff_vbi_teletext_render_v210((uint8_t *)row, ctx->fd.GetRasterWidth(),
                                    out.packet[f], &ctx->tt.cfg);
    }
}

static int aja_write_video(AVFormatContext *avctx, struct ajantv2_ctx *ctx,
                           AVPacket *pkt, int stored_new)
{
    AVFrame *frame = NULL;
    const uint8_t *src;
    int src_stride, i;
    uint8_t *dst;

    if (pkt->size == sizeof(AVFrame *))
        frame = *(AVFrame **)pkt->data;      /* AV_CODEC_ID_WRAPPED_AVFRAME */

    /* The active picture starts after the VANC rows, so copy into the visible
     * part of the buffer and leave the blanking to the teletext renderer. */
    dst = (uint8_t *)ctx->fd.GetWriteableRowAddress(ctx->host_buffer.GetHostPointer(),
                                                    ctx->fd.GetFirstActiveLine());
    if (!dst)
        return AVERROR(EIO);

    if (frame) {
        src = frame->data[0];
        src_stride = frame->linesize[0];
    } else {
        src = pkt->data;
        src_stride = (int)ctx->fd.GetBytesPerRow();
    }
    for (i = 0; i < (int)ctx->fd.GetVisibleRasterHeight(); i++)
        memcpy(dst + i * ctx->fd.GetBytesPerRow(),
               src + (size_t)i * src_stride, ctx->fd.GetBytesPerRow());

    if (ctx->teletext_enabled)
        aja_insert_teletext(avctx, ctx, stored_new);

    /* Wait for room in the ring, then hand the frame over. */
    for (;;) {
        AUTOCIRCULATE_STATUS st;
        if (!ctx->device.AutoCirculateGetStatus(ctx->channel, st))
            return AVERROR(EIO);
        if (st.CanAcceptMoreOutputFrames())
            break;
        ctx->device.WaitForOutputVerticalInterrupt(ctx->channel);
    }

    ctx->xfer.SetVideoBuffer((ULWord *)ctx->host_buffer.GetHostPointer(),
                             ctx->frame_size);
    if (!ctx->device.AutoCirculateTransfer(ctx->channel, ctx->xfer)) {
        av_log(avctx, AV_LOG_ERROR, "AutoCirculateTransfer failed\n");
        return AVERROR(EIO);
    }
    ctx->frames_out++;
    return 0;
}

extern "C" {

int ff_ajantv2_write_header(AVFormatContext *avctx)
{
    struct ajantv2_cctx *cctx = (struct ajantv2_cctx *)avctx->priv_data;
    struct ajantv2_ctx *ctx;
    AVStream *video_st = NULL;
    unsigned i;
    int ret;

    ctx = new (std::nothrow) ajantv2_ctx();
    if (!ctx)
        return AVERROR(ENOMEM);
    cctx->ctx = ctx;
    ctx->last_pts = AV_NOPTS_VALUE;
    ctx->channel = (NTV2Channel)(cctx->channel > 0 ? cctx->channel - 1 : 0);

    /* -output takes a comma-separated list of 1-based SDI connectors. Empty
     * means the connector matching the framestore, which is the usual case. */
    if (cctx->output && *cctx->output) {
        const char *p = cctx->output;
        while (*p) {
            char *end = NULL;
            long v = strtol(p, &end, 10);
            if (end == p) {
                av_log(avctx, AV_LOG_ERROR, "Cannot parse -output '%s'\n", cctx->output);
                ret = AVERROR(EINVAL);
                goto fail;
            }
            if (v < 1 || v > 8) {
                av_log(avctx, AV_LOG_ERROR, "SDI output %ld out of range 1-8\n", v);
                ret = AVERROR(EINVAL);
                goto fail;
            }
            ctx->outputs.push_back((NTV2Channel)(v - 1));
            p = end;
            while (*p == ',' || *p == ' ')
                p++;
        }
    }
    if (ctx->outputs.empty())
        ctx->outputs.push_back(ctx->channel);

    for (i = 0; i < avctx->nb_streams; i++) {
        AVStream *st = avctx->streams[i];
        switch (st->codecpar->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            video_st = st;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            if (st->codecpar->codec_id == AV_CODEC_ID_DVB_TELETEXT) {
                ctx->teletext_st = st;
                ctx->teletext_enabled = 1;
            } else {
                av_log(avctx, AV_LOG_WARNING,
                       "Ignoring subtitle stream %u: only DVB teletext is output\n", i);
            }
            break;
        case AVMEDIA_TYPE_AUDIO:
            av_log(avctx, AV_LOG_WARNING,
                   "Ignoring audio stream %u: audio output is not implemented yet\n", i);
            break;
        default:
            break;
        }
    }
    if (!video_st) {
        av_log(avctx, AV_LOG_ERROR, "A video stream is required\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }

    if (!CNTV2DeviceScanner::GetFirstDeviceFromArgument(avctx->url ? avctx->url : "0",
                                                        ctx->device)) {
        av_log(avctx, AV_LOG_ERROR, "No AJA device matching '%s'\n",
               avctx->url ? avctx->url : "0");
        ret = AVERROR(ENODEV);
        goto fail;
    }
    if (!ctx->device.IsDeviceReady(false)) {
        av_log(avctx, AV_LOG_ERROR, "AJA device is not ready\n");
        ret = AVERROR(EIO);
        goto fail;
    }
    ctx->device.AcquireStreamForApplication(AJA_FOURCC('f','f','m','p'),
                                            (int32_t)getpid());

    if ((ret = aja_setup_video(avctx, ctx, cctx, video_st)) < 0)
        goto fail;

    if (ctx->teletext_enabled) {
        FFVbiTeletextConfig tcfg;
        memset(&tcfg, 0, sizeof(tcfg));
        tcfg.vbi_offset      = cctx->teletext_vbi_offset;
        tcfg.vbi_offset_frac = cctx->teletext_vbi_offset_frac;
        tcfg.shape           = cctx->teletext_shape;
        tcfg.rise_ns         = cctx->teletext_rise_ns;
        tcfg.level           = cctx->teletext_level;
        tcfg.continuous      = cctx->teletext_continuous;
        tcfg.burst_frames    = cctx->teletext_burst_frames;
        tcfg.blank_idle      = cctx->teletext_blank_idle;
        tcfg.dual_field      = cctx->teletext_dual_field;
        tcfg.both_fields     = cctx->teletext_fields == 0;
        tcfg.defer_erase     = cctx->teletext_defer_erase;
        tcfg.filler          = cctx->teletext_filler;
        tcfg.filler_ctrl     = cctx->teletext_filler_ctrl;
        tcfg.filler_subcode  = cctx->teletext_filler_subcode;
        tcfg.spare_p8ff      = cctx->teletext_spare_p8ff;
        tcfg.header_attr     = cctx->teletext_header_attr;
        {   /* The OP-42 s7 cleardown period, in frames of this mode. */
            const NTV2FrameRate fr(::GetNTV2FrameRateFromVideoFormat(ctx->video_format));
            const double fps(::GetFramesPerSecond(fr));
            tcfg.frames_per_10s = fps > 1.0 ? (int)(fps * 10.0 + 0.5) : 250;
        }
        ff_vbi_teletext_init(&ctx->tt, &tcfg);
    }
    return 0;

fail:
    delete ctx;
    cctx->ctx = NULL;
    return ret;
}

int ff_ajantv2_write_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    struct ajantv2_cctx *cctx = (struct ajantv2_cctx *)avctx->priv_data;
    struct ajantv2_ctx *ctx = (struct ajantv2_ctx *)cctx->ctx;
    AVStream *st = avctx->streams[pkt->stream_index];

    if (!ctx)
        return AVERROR(EINVAL);

    if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
        /* The muxer interleaves by pts, so a caption that arrives here is due
         * now and can go straight into the generator. The DeckLink output
         * queues instead because it pre-renders ahead of the clock; this path
         * has no equivalent yet. */
        if (ctx->teletext_enabled)
            ff_vbi_teletext_store(&ctx->tt, pkt->data, pkt->size,
                                  pkt->pts, pkt->duration, 1, avctx);
        return 0;
    }
    if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
        return 0;

    ctx->last_pts = FFMAX(ctx->last_pts == AV_NOPTS_VALUE ? pkt->pts : ctx->last_pts,
                          pkt->pts);
    return aja_write_video(avctx, ctx, pkt, 0);
}

int ff_ajantv2_write_trailer(AVFormatContext *avctx)
{
    struct ajantv2_cctx *cctx = (struct ajantv2_cctx *)avctx->priv_data;
    struct ajantv2_ctx *ctx = (struct ajantv2_ctx *)cctx->ctx;

    if (!ctx)
        return 0;
    av_log(avctx, AV_LOG_INFO, "AJA output: %d frames sent\n", ctx->frames_out);
    ctx->device.AutoCirculateStop(ctx->channel);
    ctx->device.ReleaseStreamForApplication(AJA_FOURCC('f','f','m','p'),
                                            (int32_t)getpid());
    delete ctx;
    cctx->ctx = NULL;
    return 0;
}

} /* extern "C" */
