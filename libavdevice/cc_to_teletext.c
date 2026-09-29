/*
 * CEA-608 captions to WST teletext, for output devices
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

#include "cc_to_teletext.h"

#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/error.h"

/* A teletext page of subtitle is a header plus a handful of rows, each a 46
 * byte data unit, so this is several times what a page needs. */
#define CC_TT_BUF_SIZE 4096

int ff_cc_to_teletext_open(FFCCToTeletext *c, AVDictionary **enc_opts,
                           void *logctx)
{
    const AVCodec *dec, *enc;
    int ret;

    memset(c, 0, sizeof(*c));

    dec = avcodec_find_decoder(AV_CODEC_ID_EIA_608);
    if (!dec) {
        av_log(logctx, AV_LOG_ERROR, "No EIA-608 decoder in this build\n");
        return AVERROR_DECODER_NOT_FOUND;
    }
    enc = avcodec_find_encoder(AV_CODEC_ID_DVB_TELETEXT);
    if (!enc) {
        av_log(logctx, AV_LOG_ERROR, "No teletext encoder in this build\n");
        return AVERROR_ENCODER_NOT_FOUND;
    }

    c->dec = avcodec_alloc_context3(dec);
    c->enc = avcodec_alloc_context3(enc);
    c->buf = av_malloc(CC_TT_BUF_SIZE);
    if (!c->dec || !c->enc || !c->buf) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    c->buf_size = CC_TT_BUF_SIZE;

    /* The decoder reports caption times against this. */
    c->dec->pkt_timebase = (AVRational){1, 1000};

    if ((ret = avcodec_open2(c->dec, dec, NULL)) < 0) {
        av_log(logctx, AV_LOG_ERROR, "Cannot open the EIA-608 decoder\n");
        goto fail;
    }
    if ((ret = avcodec_open2(c->enc, enc, enc_opts)) < 0) {
        av_log(logctx, AV_LOG_ERROR, "Cannot open the teletext encoder\n");
        goto fail;
    }
    return 0;

fail:
    ff_cc_to_teletext_close(c);
    return ret;
}

void ff_cc_to_teletext_close(FFCCToTeletext *c)
{
    avcodec_free_context(&c->dec);
    avcodec_free_context(&c->enc);
    av_freep(&c->buf);
    c->buf_size = 0;
}

int ff_cc_to_teletext(FFCCToTeletext *c, const uint8_t *cc, int cc_size,
                      int64_t pts, AVRational tb, const uint8_t **out,
                      void *logctx)
{
    AVSubtitle sub = {0};
    AVPacket *pkt;
    int got = 0, ret;

    *out = NULL;
    if (!c->dec || !c->enc || cc_size <= 0)
        return 0;

    pkt = av_packet_alloc();
    if (!pkt)
        return AVERROR(ENOMEM);
    if ((ret = av_new_packet(pkt, cc_size)) < 0) {
        av_packet_free(&pkt);
        return ret;
    }
    memcpy(pkt->data, cc, cc_size);
    pkt->pts = av_rescale_q(pts, tb, c->dec->pkt_timebase);

    ret = avcodec_decode_subtitle2(c->dec, &sub, &got, pkt);
    av_packet_free(&pkt);
    if (ret < 0) {
        av_log(logctx, AV_LOG_WARNING, "Cannot decode the captions on a frame\n");
        return 0;
    }
    if (!got)
        return 0;               /* the caption has not finished arriving */

    ret = avcodec_encode_subtitle(c->enc, c->buf, c->buf_size, &sub);
    avsubtitle_free(&sub);
    if (ret <= 0) {
        if (ret < 0)
            av_log(logctx, AV_LOG_WARNING, "Cannot build a teletext page\n");
        return 0;
    }

    *out = c->buf;
    return ret;
}
