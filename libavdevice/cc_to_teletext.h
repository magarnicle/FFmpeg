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

#ifndef AVDEVICE_CC_TO_TELETEXT_H
#define AVDEVICE_CC_TO_TELETEXT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "libavcodec/avcodec.h"
#include "libavutil/dict.h"

/*
 * Turns the CEA-608 caption bytes carried on a video frame into the teletext
 * data units an SD PAL output puts on the VBI.
 *
 * It exists so a device can take captions on the video stream rather than as a
 * separate subtitle stream. A subtitle stream is sparse -- one packet per
 * caption -- and ffmpeg's muxer holds every other stream until each interleaved
 * stream has a packet to compare (mux.c, nb_interleaved_streams), so a feed with
 * captions seconds apart stalls the picture for seconds at a time, and a feed
 * with no captions at all stalls it until max_interleave_delta expires. Captions
 * riding on the video frames as A53 side data are never sparse, because the
 * video is not.
 *
 * The conversion is the same pair of codecs a command line would use,
 * eia_608 -> cc_dec -> ASS -> teletextenc -> WST, opened here instead of by
 * ffmpeg's stream mapping. Nothing is reimplemented, so a fix in either codec
 * reaches both routes.
 */
typedef struct FFCCToTeletext {
    AVCodecContext *dec;        /* EIA-608 to ASS */
    AVCodecContext *enc;        /* ASS to DVB teletext data units */
    uint8_t        *buf;        /* encoder output, reused */
    int             buf_size;
} FFCCToTeletext;

/* enc_opts are the teletext encoder's own options: page, magazine,
 * double_height and the rest. NULL takes its defaults. */
int  ff_cc_to_teletext_open(FFCCToTeletext *c, AVDictionary **enc_opts,
                            void *logctx);

void ff_cc_to_teletext_close(FFCCToTeletext *c);

/* cc is the A53 side data from a video packet: cc_count triplets. Returns the
 * number of bytes of teletext in *out, which points into the converter and
 * stays valid until the next call, or 0 when this frame's captions did not
 * complete a page. */
int  ff_cc_to_teletext(FFCCToTeletext *c, const uint8_t *cc, int cc_size,
                       int64_t pts, AVRational tb, const uint8_t **out,
                       void *logctx);

#ifdef __cplusplus
}
#endif

#endif /* AVDEVICE_CC_TO_TELETEXT_H */
