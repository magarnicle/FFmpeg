/*
 * Copyright 2011 Stefano Sabatini <stefano.sabatini-lala poste it>
 * Copyright 2012 Nicolas George <nicolas.george normalesup org>
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

#include <string.h>
#include <inttypes.h>

#include "libavutil/avassert.h"
#include "libavutil/avutil.h"
#include "libavutil/cpu.h"
#include "libavutil/csp.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"
#include "libavutil/log.h"
#include "colorspace.h"
#include "drawutils.h"
#include "formats.h"

#if ARCH_X86 && HAVE_AVX2_INLINE
#include <immintrin.h>

/*
 * Blend one row of 16-bit luma pixels (hsub=0, vsub=0, pixelstep=2) using
 * an 8-bit grayscale mask.  Formula per pixel:
 *   out = ((0x10001 - a) * v + a * src) >> 16,  a = mask[xm+x] * alpha
 * Safe for pixel depths < 16 bit (intermediates fit in uint32).
 */
/* Shared 4-pixel SSE4.1 blend kernel (128-bit): called from both luma and chroma. */
#define BLEND4_SSE(dst4, src4, alpha4, v10001_4, v32, a32)             \
    do {                                                                 \
        __m128i _tau = _mm_sub_epi32(v10001_4, a32);                   \
        __m128i _res = _mm_srli_epi32(                                  \
                           _mm_add_epi32(_mm_mullo_epi32(_tau, v32),    \
                                         _mm_mullo_epi32(a32, src4)),   \
                           16);                                          \
        _mm_storel_epi64((__m128i *)(dst4),                             \
                         _mm_packus_epi32(_res, _res));                 \
    } while (0)

__attribute__((target("avx2")))
static void blend_line16_luma_avx2(uint16_t *dst, unsigned src, unsigned alpha,
                                    const uint8_t *mask, int xm, int w)
{
    const __m256i vsrc    = _mm256_set1_epi32(src);
    const __m256i valpha  = _mm256_set1_epi32(alpha);
    const __m256i v10001  = _mm256_set1_epi32(0x10001);
    const __m128i vsrc4   = _mm_set1_epi32(src);
    const __m128i valpha4 = _mm_set1_epi32(alpha);
    const __m128i v10001_4= _mm_set1_epi32(0x10001);
    int x = 0;

    for (; x <= w - 8; x += 8) {
        __m256i v32  = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i *)(dst + x)));
        __m256i m32  = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(mask + xm + x)));
        __m256i a32  = _mm256_mullo_epi32(m32, valpha);
        __m256i tau  = _mm256_sub_epi32(v10001, a32);
        __m256i res  = _mm256_srli_epi32(
                           _mm256_add_epi32(_mm256_mullo_epi32(tau, v32),
                                            _mm256_mullo_epi32(a32, vsrc)),
                           16);
        __m128i out  = _mm_packus_epi32(_mm256_castsi256_si128(res),
                                         _mm256_extracti128_si256(res, 1));
        _mm_storeu_si128((__m128i *)(dst + x), out);
    }
    if (x <= w - 4) {
        __m128i v32 = _mm_cvtepu16_epi32(_mm_loadl_epi64((const __m128i *)(dst + x)));
        __m128i m32 = _mm_cvtepu8_epi32(_mm_cvtsi32_si128(*(const int *)(mask + xm + x)));
        __m128i a32 = _mm_mullo_epi32(m32, valpha4);
        BLEND4_SSE(dst + x, vsrc4, valpha4, v10001_4, v32, a32);
        x += 4;
    }
    for (; x < w; x++) {
        unsigned a = mask[xm + x] * alpha;
        unsigned v = dst[x];
        dst[x] = ((0x10001 - a) * v + a * src) >> 16;
    }
}

/*
 * Blend one row of 16-bit 4:2:2 chroma pixels (hsub=1, vsub=0, pixelstep=2).
 * Each chroma pixel averages two horizontally adjacent mask bytes.
 */
__attribute__((target("avx2")))
static void blend_line16_chroma422_avx2(uint16_t *dst, unsigned src, unsigned alpha,
                                         const uint8_t *mask, int xm, int w)
{
    const __m256i vsrc    = _mm256_set1_epi32(src);
    const __m256i valpha  = _mm256_set1_epi32(alpha);
    const __m256i v10001  = _mm256_set1_epi32(0x10001);
    const __m128i vsrc4   = _mm_set1_epi32(src);
    const __m128i valpha4 = _mm_set1_epi32(alpha);
    const __m128i v10001_4= _mm_set1_epi32(0x10001);
    const __m128i ones8   = _mm_set1_epi8(1);
    int x = 0;

    for (; x <= w - 8; x += 8) {
        /* Sum adjacent mask byte pairs: _mm_maddubs_epi16 with coeff=1 */
        __m128i m16b = _mm_loadu_si128((const __m128i *)(mask + xm + x * 2));
        __m128i sums = _mm_maddubs_epi16(m16b, ones8);   /* 8 x uint16 sums */
        __m256i s32  = _mm256_cvtepu16_epi32(sums);
        /* a = (sum >> 1) * alpha */
        __m256i a32  = _mm256_mullo_epi32(_mm256_srli_epi32(s32, 1), valpha);
        __m256i v32  = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i *)(dst + x)));
        __m256i tau  = _mm256_sub_epi32(v10001, a32);
        __m256i res  = _mm256_srli_epi32(
                           _mm256_add_epi32(_mm256_mullo_epi32(tau, v32),
                                            _mm256_mullo_epi32(a32, vsrc)),
                           16);
        __m128i out  = _mm_packus_epi32(_mm256_castsi256_si128(res),
                                         _mm256_extracti128_si256(res, 1));
        _mm_storeu_si128((__m128i *)(dst + x), out);
    }
    if (x <= w - 4) {
        /* 4-pixel SSE4.1 path for chroma remainder */
        __m128i m8b  = _mm_loadl_epi64((const __m128i *)(mask + xm + x * 2));
        __m128i sums = _mm_maddubs_epi16(m8b, ones8);    /* 4 x uint16 sums (lo 64 bits) */
        __m128i s32  = _mm_cvtepu16_epi32(sums);
        __m128i a32  = _mm_mullo_epi32(_mm_srli_epi32(s32, 1), valpha4);
        __m128i v32  = _mm_cvtepu16_epi32(_mm_loadl_epi64((const __m128i *)(dst + x)));
        BLEND4_SSE(dst + x, vsrc4, valpha4, v10001_4, v32, a32);
        x += 4;
    }
    for (; x < w; x++) {
        unsigned t = mask[xm + x*2] + mask[xm + x*2 + 1];
        unsigned a = (t >> 1) * alpha;
        unsigned v = dst[x];
        dst[x] = ((0x10001 - a) * v + a * src) >> 16;
    }
}
#if HAVE_AVX512_INLINE
/*
 * AVX-512 variants: 16 pixels per iteration.
 * Requires avx512f (for _mm512_cvtepu8/16_epi32, _mm512_cvtusepi32_epi16)
 * and avx512bw (for _mm256_maddubs_epi16 within the chroma path).
 */
__attribute__((target("avx512f,avx512bw")))
static void blend_line16_luma_avx512(uint16_t *dst, unsigned src, unsigned alpha,
                                      const uint8_t *mask, int xm, int w)
{
    const __m512i vsrc512   = _mm512_set1_epi32(src);
    const __m512i valpha512 = _mm512_set1_epi32(alpha);
    const __m512i v10001_512= _mm512_set1_epi32(0x10001);
    const __m256i vsrc256   = _mm256_set1_epi32(src);
    const __m256i valpha256 = _mm256_set1_epi32(alpha);
    const __m256i v10001_256= _mm256_set1_epi32(0x10001);
    int x = 0;

    for (; x <= w - 16; x += 16) {
        __m512i v32 = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(dst + x)));
        __m512i m32 = _mm512_cvtepu8_epi32(_mm_loadu_si128((const __m128i *)(mask + xm + x)));
        __m512i a32 = _mm512_mullo_epi32(m32, valpha512);
        __m512i tau = _mm512_sub_epi32(v10001_512, a32);
        __m512i res = _mm512_srli_epi32(
                          _mm512_add_epi32(_mm512_mullo_epi32(tau, v32),
                                           _mm512_mullo_epi32(a32, vsrc512)),
                          16);
        _mm256_storeu_si256((__m256i *)(dst + x), _mm512_cvtusepi32_epi16(res));
    }
    for (; x <= w - 8; x += 8) {
        __m256i v32 = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i *)(dst + x)));
        __m256i m32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(mask + xm + x)));
        __m256i a32 = _mm256_mullo_epi32(m32, valpha256);
        __m256i tau = _mm256_sub_epi32(v10001_256, a32);
        __m256i res = _mm256_srli_epi32(
                          _mm256_add_epi32(_mm256_mullo_epi32(tau, v32),
                                           _mm256_mullo_epi32(a32, vsrc256)),
                          16);
        _mm_storeu_si128((__m128i *)(dst + x),
                         _mm_packus_epi32(_mm256_castsi256_si128(res),
                                          _mm256_extracti128_si256(res, 1)));
    }
    for (; x < w; x++) {
        unsigned a = mask[xm + x] * alpha;
        unsigned v = dst[x];
        dst[x] = ((0x10001 - a) * v + a * src) >> 16;
    }
}

__attribute__((target("avx512f,avx512bw")))
static void blend_line16_chroma422_avx512(uint16_t *dst, unsigned src, unsigned alpha,
                                           const uint8_t *mask, int xm, int w)
{
    const __m512i vsrc512    = _mm512_set1_epi32(src);
    const __m512i valpha512  = _mm512_set1_epi32(alpha);
    const __m512i v10001_512 = _mm512_set1_epi32(0x10001);
    const __m256i vsrc256    = _mm256_set1_epi32(src);
    const __m256i valpha256  = _mm256_set1_epi32(alpha);
    const __m256i v10001_256 = _mm256_set1_epi32(0x10001);
    const __m256i ones8_256  = _mm256_set1_epi8(1);
    const __m128i ones8_128  = _mm_set1_epi8(1);
    int x = 0;

    for (; x <= w - 16; x += 16) {
        /* Sum 32 adjacent mask byte pairs → 16 uint16 sums */
        __m256i m32b = _mm256_loadu_si256((const __m256i *)(mask + xm + x * 2));
        __m256i sums = _mm256_maddubs_epi16(m32b, ones8_256);  /* 16 x uint16 */
        __m512i s32  = _mm512_cvtepu16_epi32(sums);
        __m512i a32  = _mm512_mullo_epi32(_mm512_srli_epi32(s32, 1), valpha512);
        __m512i v32  = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(dst + x)));
        __m512i tau  = _mm512_sub_epi32(v10001_512, a32);
        __m512i res  = _mm512_srli_epi32(
                           _mm512_add_epi32(_mm512_mullo_epi32(tau, v32),
                                            _mm512_mullo_epi32(a32, vsrc512)),
                           16);
        _mm256_storeu_si256((__m256i *)(dst + x), _mm512_cvtusepi32_epi16(res));
    }
    for (; x <= w - 8; x += 8) {
        __m128i m16b = _mm_loadu_si128((const __m128i *)(mask + xm + x * 2));
        __m128i sums = _mm_maddubs_epi16(m16b, ones8_128);
        __m256i s32  = _mm256_cvtepu16_epi32(sums);
        __m256i a32  = _mm256_mullo_epi32(_mm256_srli_epi32(s32, 1), valpha256);
        __m256i v32  = _mm256_cvtepu16_epi32(_mm_loadu_si128((const __m128i *)(dst + x)));
        __m256i tau  = _mm256_sub_epi32(v10001_256, a32);
        __m256i res  = _mm256_srli_epi32(
                           _mm256_add_epi32(_mm256_mullo_epi32(tau, v32),
                                            _mm256_mullo_epi32(a32, vsrc256)),
                           16);
        _mm_storeu_si128((__m128i *)(dst + x),
                         _mm_packus_epi32(_mm256_castsi256_si128(res),
                                          _mm256_extracti128_si256(res, 1)));
    }
    for (; x < w; x++) {
        unsigned t = mask[xm + x*2] + mask[xm + x*2 + 1];
        unsigned a = (t >> 1) * alpha;
        unsigned v = dst[x];
        dst[x] = ((0x10001 - a) * v + a * src) >> 16;
    }
}
#endif /* HAVE_AVX512_INLINE */

#endif /* ARCH_X86 && HAVE_AVX2_INLINE */

enum { RED = 0, GREEN, BLUE, ALPHA };

static int fill_map(const AVPixFmtDescriptor *desc, uint8_t *map)
{
    if (desc->flags & (AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL |
                       AV_PIX_FMT_FLAG_BAYER | AV_PIX_FMT_FLAG_XYZ | AV_PIX_FMT_FLAG_PAL))
        return AVERROR(EINVAL);
    av_assert0(desc->nb_components == 3 + !!(desc->flags & AV_PIX_FMT_FLAG_ALPHA));
    if (desc->flags & AV_PIX_FMT_FLAG_PLANAR) {
        if (desc->nb_components != av_pix_fmt_count_planes(av_pix_fmt_desc_get_id(desc)))
            return AVERROR(EINVAL);
        map[RED]   = desc->comp[0].plane;
        map[GREEN] = desc->comp[1].plane;
        map[BLUE]  = desc->comp[2].plane;
        map[ALPHA] = (desc->flags & AV_PIX_FMT_FLAG_ALPHA) ? desc->comp[3].plane : 3;
    } else {
        int had0 = 0;
        unsigned depthb = 0;
        for (unsigned i = 0; i < desc->nb_components; i++) {
            /* all components must have same depth in bytes */
            unsigned db = (desc->comp[i].depth + 7) / 8;
            unsigned pos = desc->comp[i].offset / db;
            if (depthb && (depthb != db))
                return AVERROR(ENOSYS);

            if (desc->comp[i].offset % db)
                return AVERROR(ENOSYS);

            had0 |= pos == 0;
            map[i] = pos;
            depthb = db;
        }

        if (desc->nb_components == 3)
            map[ALPHA] = had0 ? 3 : 0;
    }

    av_assert0(map[RED]   != map[GREEN]);
    av_assert0(map[GREEN] != map[BLUE]);
    av_assert0(map[BLUE]  != map[RED]);
    av_assert0(map[RED]   != map[ALPHA]);
    av_assert0(map[GREEN] != map[ALPHA]);
    av_assert0(map[BLUE]  != map[ALPHA]);

    return 0;
}

int ff_fill_rgba_map(uint8_t *rgba_map, enum AVPixelFormat pix_fmt)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pix_fmt);
    if (!(desc->flags & AV_PIX_FMT_FLAG_RGB))
        return AVERROR(EINVAL);
    return fill_map(desc, rgba_map);
}

int ff_fill_ayuv_map(uint8_t *ayuv_map, enum AVPixelFormat pix_fmt)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pix_fmt);
    if (desc->flags & AV_PIX_FMT_FLAG_RGB)
        return AVERROR(EINVAL);
    return fill_map(desc, ayuv_map);
}

int ff_draw_init2(FFDrawContext *draw, enum AVPixelFormat format, enum AVColorSpace csp,
                  enum AVColorRange range, enum AVAlphaMode alpha, unsigned flags)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(format);
    const AVLumaCoefficients *luma = NULL;
    const AVComponentDescriptor *c;
    unsigned nb_planes = 0;
    int pixelstep[MAX_PLANES] = { 0 };
    int depthb = 0;

    if (!desc || !desc->name)
        return AVERROR(EINVAL);
    if (desc->flags & AV_PIX_FMT_FLAG_BE)
        return AVERROR(ENOSYS);
    if (desc->flags & ~(AV_PIX_FMT_FLAG_PLANAR | AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_ALPHA))
        return AVERROR(ENOSYS);
    if (csp == AVCOL_SPC_UNSPECIFIED)
        csp = (desc->flags & AV_PIX_FMT_FLAG_RGB) ? AVCOL_SPC_RGB : AVCOL_SPC_SMPTE170M;
    if (!(desc->flags & AV_PIX_FMT_FLAG_RGB) && !(luma = av_csp_luma_coeffs_from_avcsp(csp)))
        return AVERROR(EINVAL);
    if (range == AVCOL_RANGE_UNSPECIFIED)
        range = (format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P ||
                 format == AV_PIX_FMT_YUVJ444P || format == AV_PIX_FMT_YUVJ411P ||
                 format == AV_PIX_FMT_YUVJ440P || csp == AVCOL_SPC_RGB)
                ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    if (range != AVCOL_RANGE_JPEG && range != AVCOL_RANGE_MPEG)
        return AVERROR(EINVAL);
    for (unsigned i = 0; i < desc->nb_components; i++) {
        int db;
        c = &desc->comp[i];
        /* for now, only 8-16 bits formats */
        if (c->depth < 8 || c->depth > 16)
            return AVERROR(ENOSYS);
        if (c->plane >= MAX_PLANES)
            return AVERROR(ENOSYS);
        /* data must either be in the high or low bits, never middle */
        if (c->shift && ((c->shift + c->depth) & 0x7))
            return AVERROR(ENOSYS);
        /* mixed >8 and <=8 depth */
        db = (c->depth + 7) / 8;
        if (depthb && (depthb != db))
            return AVERROR(ENOSYS);
        depthb = db;
        if (db * (c->offset + 1) > 16)
            return AVERROR(ENOSYS);
        if (c->offset % db)
            return AVERROR(ENOSYS);
        /* strange interleaving */
        if (pixelstep[c->plane] != 0 &&
            pixelstep[c->plane] != c->step)
            return AVERROR(ENOSYS);
        pixelstep[c->plane] = c->step;
        if (pixelstep[c->plane] >= 8)
            return AVERROR(ENOSYS);
        nb_planes = FFMAX(nb_planes, c->plane + 1);
    }
    memset(draw, 0, sizeof(*draw));
    draw->desc      = desc;
    draw->format    = format;
    draw->nb_planes = nb_planes;
    draw->range     = range;
    draw->csp       = csp;
    draw->alpha     = alpha;
    draw->flags     = flags;
    if (luma)
        ff_fill_rgb2yuv_table(luma, draw->rgb2yuv);
    memcpy(draw->pixelstep, pixelstep, sizeof(draw->pixelstep));
    draw->hsub[1] = draw->hsub[2] = draw->hsub_max = desc->log2_chroma_w;
    draw->vsub[1] = draw->vsub[2] = draw->vsub_max = desc->log2_chroma_h;
    return 0;
}

int ff_draw_init_from_link(FFDrawContext *draw, const AVFilterLink *link,
                           unsigned flags)
{
    return ff_draw_init2(draw, link->format, link->colorspace, link->color_range, link->alpha_mode, flags);
}

int ff_draw_init(FFDrawContext *draw, enum AVPixelFormat format, unsigned flags)
{
    return ff_draw_init2(draw, format, AVCOL_SPC_UNSPECIFIED, AVCOL_RANGE_UNSPECIFIED, AVALPHA_MODE_UNSPECIFIED, flags);
}

void ff_draw_color(FFDrawContext *draw, FFDrawColor *color, const uint8_t rgba[4])
{
    double yuvad[4];
    double rgbad[4];
    const AVPixFmtDescriptor *desc = draw->desc;

    if (rgba != color->rgba)
        memcpy(color->rgba, rgba, sizeof(color->rgba));

    memset(color->comp, 0, sizeof(color->comp));

    for (int i = 0; i < 4; i++)
        rgbad[i] = color->rgba[i] / 255.;

    if (draw->alpha == AVALPHA_MODE_PREMULTIPLIED) {
        for (int i = 0; i < 3; i++)
            rgbad[i] *= rgbad[3];
    }

    if (draw->desc->flags & AV_PIX_FMT_FLAG_RGB)
        memcpy(yuvad, rgbad, sizeof(double) * 3);
    else
        ff_matrix_mul_3x3_vec(yuvad, rgbad, draw->rgb2yuv);

    yuvad[3] = rgbad[3];

    for (int i = 0; i < 3; i++) {
        int chroma = (!(draw->desc->flags & AV_PIX_FMT_FLAG_RGB) && i > 0);
        if (draw->range == AVCOL_RANGE_MPEG) {
            yuvad[i] *= (chroma ? 224. : 219.) / 255.;
            yuvad[i] += (chroma ? 128. :  16.) / 255.;
        } else if (chroma) {
            yuvad[i] += 0.5;
        }
    }

    // Ensure we place the alpha appropriately for gray formats
    if (desc->nb_components <= 2)
        yuvad[1] = yuvad[3];

    for (unsigned i = 0; i < desc->nb_components; i++) {
        unsigned val = yuvad[i] * ((1 << (draw->desc->comp[i].depth + draw->desc->comp[i].shift)) - 1) + 0.5;
        if (desc->comp[i].depth > 8)
            color->comp[desc->comp[i].plane].u16[desc->comp[i].offset / 2] = val;
        else
            color->comp[desc->comp[i].plane].u8[desc->comp[i].offset] = val;
    }
}

static uint8_t *pointer_at(FFDrawContext *draw, uint8_t *data[], int linesize[],
                           int plane, int x, int y)
{
    return data[plane] +
           (y >> draw->vsub[plane]) * linesize[plane] +
           (x >> draw->hsub[plane]) * draw->pixelstep[plane];
}

void ff_copy_rectangle2(FFDrawContext *draw,
                        uint8_t *dst[], int dst_linesize[],
                        uint8_t *src[], int src_linesize[],
                        int dst_x, int dst_y, int src_x, int src_y,
                        int w, int h)
{
    int wp, hp;
    uint8_t *p, *q;

    for (int plane = 0; plane < draw->nb_planes; plane++) {
        p = pointer_at(draw, src, src_linesize, plane, src_x, src_y);
        q = pointer_at(draw, dst, dst_linesize, plane, dst_x, dst_y);
        wp = AV_CEIL_RSHIFT(w, draw->hsub[plane]) * draw->pixelstep[plane];
        hp = AV_CEIL_RSHIFT(h, draw->vsub[plane]);
        for (int y = 0; y < hp; y++) {
            memcpy(q, p, wp);
            p += src_linesize[plane];
            q += dst_linesize[plane];
        }
    }
}

void ff_fill_rectangle(FFDrawContext *draw, FFDrawColor *color,
                       uint8_t *dst[], int dst_linesize[],
                       int dst_x, int dst_y, int w, int h)
{
    int wp, hp;
    uint8_t *p0, *p;
    FFDrawColor color_tmp = *color;

    for (int plane = 0; plane < draw->nb_planes; plane++) {
        p0 = pointer_at(draw, dst, dst_linesize, plane, dst_x, dst_y);
        wp = AV_CEIL_RSHIFT(w, draw->hsub[plane]);
        hp = AV_CEIL_RSHIFT(h, draw->vsub[plane]);
        if (!hp)
            return;
        p = p0;

        if (HAVE_BIGENDIAN && draw->desc->comp[0].depth > 8) {
            for (int x = 0; 2*x < draw->pixelstep[plane]; x++)
                color_tmp.comp[plane].u16[x] = av_bswap16(color_tmp.comp[plane].u16[x]);
        }

        /* copy first line from color */
        for (int x = 0; x < wp; x++) {
            memcpy(p, color_tmp.comp[plane].u8, draw->pixelstep[plane]);
            p += draw->pixelstep[plane];
        }
        wp *= draw->pixelstep[plane];
        /* copy next lines from first line */
        p = p0 + dst_linesize[plane];
        for (int y = 1; y < hp; y++) {
            memcpy(p, p0, wp);
            p += dst_linesize[plane];
        }
    }
}

/**
 * Clip interval [x; x+w[ within [0; wmax[.
 * The resulting w may be negative if the final interval is empty.
 * dx, if not null, return the difference between in and out value of x.
 */
static void clip_interval(int wmax, int *x, int *w, int *dx)
{
    if (dx)
        *dx = 0;
    if (*x < 0) {
        if (dx)
            *dx = -*x;
        *w += *x;
        *x = 0;
    }
    if (*x + *w > wmax)
        *w = wmax - *x;
}

/**
 * Decompose w pixels starting at x
 * into start + (w starting at x) + end
 * with x and w aligned on multiples of 1<<sub.
 */
static void subsampling_bounds(int sub, int *x, int *w, int *start, int *end)
{
    int mask = (1 << sub) - 1;

    *start = (-*x) & mask;
    *x += *start;
    *start = FFMIN(*start, *w);
    *w -= *start;
    *end = *w & mask;
    *w >>= sub;
}

/* If alpha is in the [ 0 ; 0x1010101 ] range,
   then alpha * value is in the [ 0 ; 0xFFFFFFFF ] range,
   and >> 24 gives a correct rounding. */
static void blend_line(uint8_t *dst, unsigned src, unsigned alpha,
                       int dx, int w, unsigned hsub, int left, int right)
{
    unsigned asrc = alpha * src;
    unsigned tau = 0x1010101 - alpha;

    if (left) {
        unsigned suba = (left * alpha) >> hsub;
        *dst = (*dst * (0x1010101 - suba) + src * suba) >> 24;
        dst += dx;
    }
    for (int x = 0; x < w; x++) {
        *dst = (*dst * tau + asrc) >> 24;
        dst += dx;
    }
    if (right) {
        unsigned suba = (right * alpha) >> hsub;
        *dst = (*dst * (0x1010101 - suba) + src * suba) >> 24;
    }
}

static void blend_line16(uint8_t *dst, unsigned src, unsigned alpha,
                         int dx, int w, unsigned hsub, int left, int right)
{
    unsigned asrc = alpha * src;
    unsigned tau = 0x10001 - alpha;

    if (left) {
        unsigned suba = (left * alpha) >> hsub;
        uint16_t value = AV_RL16(dst);
        AV_WL16(dst, (value * (0x10001 - suba) + src * suba) >> 16);
        dst += dx;
    }
    for (int x = 0; x < w; x++) {
        uint16_t value = AV_RL16(dst);
        AV_WL16(dst, (value * tau + asrc) >> 16);
        dst += dx;
    }
    if (right) {
        unsigned suba = (right * alpha) >> hsub;
        uint16_t value = AV_RL16(dst);
        AV_WL16(dst, (value * (0x10001 - suba) + src * suba) >> 16);
    }
}

void ff_blend_rectangle(FFDrawContext *draw, FFDrawColor *color,
                        uint8_t *dst[], int dst_linesize[],
                        int dst_w, int dst_h,
                        int x0, int y0, int w, int h)
{
    unsigned alpha, nb_planes, nb_comp;
    int w_sub, h_sub, x_sub, y_sub, left, right, top, bottom;
    uint8_t *p0, *p;

    nb_comp = draw->desc->nb_components -
        !!(draw->desc->flags & AV_PIX_FMT_FLAG_ALPHA && !(draw->flags & FF_DRAW_PROCESS_ALPHA));

    /* TODO optimize if alpha = 0xFF */
    clip_interval(dst_w, &x0, &w, NULL);
    clip_interval(dst_h, &y0, &h, NULL);
    if (w <= 0 || h <= 0 || !color->rgba[3])
        return;
    if (draw->desc->comp[0].depth <= 8) {
        /* 0x10203 * alpha + 2 is in the [ 2 ; 0x1010101 - 2 ] range */
        alpha = 0x10203 * color->rgba[3] + 0x2;
    } else {
        /* 0x101 * alpha is in the [ 2 ; 0x1001] range */
        alpha = 0x101 * color->rgba[3] + 0x2;
    }
    nb_planes = draw->nb_planes - !!(draw->desc->flags & AV_PIX_FMT_FLAG_ALPHA && !(draw->flags & FF_DRAW_PROCESS_ALPHA));
    nb_planes += !nb_planes;
    for (unsigned plane = 0; plane < nb_planes; plane++) {
        p0 = pointer_at(draw, dst, dst_linesize, plane, x0, y0);
        w_sub = w;
        h_sub = h;
        x_sub = x0;
        y_sub = y0;
        subsampling_bounds(draw->hsub[plane], &x_sub, &w_sub, &left, &right);
        subsampling_bounds(draw->vsub[plane], &y_sub, &h_sub, &top, &bottom);
        for (unsigned comp = 0; comp < nb_comp; comp++) {
            const int depth = draw->desc->comp[comp].depth;
            const int offset = draw->desc->comp[comp].offset;
            const int index = offset / ((depth + 7) / 8);

            if (draw->desc->comp[comp].plane != plane)
                continue;
            p = p0 + offset;
            if (top) {
                if (depth <= 8) {
                    blend_line(p, color->comp[plane].u8[index], alpha >> 1,
                               draw->pixelstep[plane], w_sub,
                               draw->hsub[plane], left, right);
                } else {
                    blend_line16(p, color->comp[plane].u16[index], alpha >> 1,
                                 draw->pixelstep[plane], w_sub,
                                 draw->hsub[plane], left, right);
                }
                p += dst_linesize[plane];
            }
            if (depth <= 8) {
                for (int y = 0; y < h_sub; y++) {
                    blend_line(p, color->comp[plane].u8[index], alpha,
                               draw->pixelstep[plane], w_sub,
                               draw->hsub[plane], left, right);
                    p += dst_linesize[plane];
                }
            } else {
                for (int y = 0; y < h_sub; y++) {
                    blend_line16(p, color->comp[plane].u16[index], alpha,
                                 draw->pixelstep[plane], w_sub,
                                 draw->hsub[plane], left, right);
                    p += dst_linesize[plane];
                }
            }
            if (bottom) {
                if (depth <= 8) {
                    blend_line(p, color->comp[plane].u8[index], alpha >> 1,
                               draw->pixelstep[plane], w_sub,
                               draw->hsub[plane], left, right);
                } else {
                    blend_line16(p, color->comp[plane].u16[index], alpha >> 1,
                                 draw->pixelstep[plane], w_sub,
                                 draw->hsub[plane], left, right);
                }
            }
        }
    }
}

static void blend_pixel16(uint8_t *dst, unsigned src, unsigned alpha,
                          const uint8_t *mask, int mask_linesize, int l2depth,
                          unsigned w, unsigned h, unsigned shift, unsigned xm0)
{
    unsigned t = 0;
    unsigned xmshf = 3 - l2depth;
    unsigned xmmod = 7 >> l2depth;
    unsigned mbits = (1 << (1 << l2depth)) - 1;
    unsigned mmult = 255 / mbits;
    uint16_t value = AV_RL16(dst);

    for (unsigned y = 0; y < h; y++) {
        unsigned xm = xm0;
        for (unsigned x = 0; x < w; x++) {
            t += ((mask[xm >> xmshf] >> ((~xm & xmmod) << l2depth)) & mbits)
                 * mmult;
            xm++;
        }
        mask += mask_linesize;
    }
    alpha = (t >> shift) * alpha;
    AV_WL16(dst, ((0x10001 - alpha) * value + alpha * src) >> 16);
}

static void blend_pixel(uint8_t *dst, unsigned src, unsigned alpha,
                        const uint8_t *mask, int mask_linesize, int l2depth,
                        unsigned w, unsigned h, unsigned shift, unsigned xm0)
{
    unsigned t = 0;
    unsigned xmshf = 3 - l2depth;
    unsigned xmmod = 7 >> l2depth;
    unsigned mbits = (1 << (1 << l2depth)) - 1;
    unsigned mmult = 255 / mbits;

    for (unsigned y = 0; y < h; y++) {
        unsigned xm = xm0;
        for (unsigned x = 0; x < w; x++) {
            t += ((mask[xm >> xmshf] >> ((~xm & xmmod) << l2depth)) & mbits)
                 * mmult;
            xm++;
        }
        mask += mask_linesize;
    }
    alpha = (t >> shift) * alpha;
    *dst = ((0x1010101 - alpha) * *dst + alpha * src) >> 24;
}

static void blend_line_hv16(uint8_t *dst, int dst_delta,
                            unsigned src, unsigned alpha,
                            const uint8_t *mask, int mask_linesize, int l2depth, int w,
                            unsigned hsub, unsigned vsub,
                            int xm, int left, int right, int hband)
{

    if (left) {
        blend_pixel16(dst, src, alpha, mask, mask_linesize, l2depth,
                      left, hband, hsub + vsub, xm);
        dst += dst_delta;
        xm += left;
    }
    for (int x = 0; x < w; x++) {
        blend_pixel16(dst, src, alpha, mask, mask_linesize, l2depth,
                      1 << hsub, hband, hsub + vsub, xm);
        dst += dst_delta;
        xm += 1 << hsub;
    }
    if (right)
        blend_pixel16(dst, src, alpha, mask, mask_linesize, l2depth,
                      right, hband, hsub + vsub, xm);
}

static void blend_line_hv(uint8_t *dst, int dst_delta,
                          unsigned src, unsigned alpha,
                          const uint8_t *mask, int mask_linesize, int l2depth, int w,
                          unsigned hsub, unsigned vsub,
                          int xm, int left, int right, int hband)
{

    if (left) {
        blend_pixel(dst, src, alpha, mask, mask_linesize, l2depth,
                    left, hband, hsub + vsub, xm);
        dst += dst_delta;
        xm += left;
    }
    for (int x = 0; x < w; x++) {
        blend_pixel(dst, src, alpha, mask, mask_linesize, l2depth,
                    1 << hsub, hband, hsub + vsub, xm);
        dst += dst_delta;
        xm += 1 << hsub;
    }
    if (right)
        blend_pixel(dst, src, alpha, mask, mask_linesize, l2depth,
                    right, hband, hsub + vsub, xm);
}

void ff_blend_mask(FFDrawContext *draw, FFDrawColor *color,
                   uint8_t *dst[], int dst_linesize[], int dst_w, int dst_h,
                   const uint8_t *mask,  int mask_linesize, int mask_w, int mask_h,
                   int l2depth, unsigned endianness, int x0, int y0)
{
    unsigned alpha, nb_planes, nb_comp;
    int xm0, ym0, w_sub, h_sub, x_sub, y_sub, left, right, top, bottom;
    uint8_t *p;
    const uint8_t *m;
    static int64_t s_blend8_time = 0, s_blend16_time = 0, s_blend16_simd_time = 0;
    static int s_blend8_count = 0, s_blend16_count = 0, s_blend16_simd_count = 0;
    static int s_log_counter = 0;

    nb_comp = draw->desc->nb_components -
        !!(draw->desc->flags & AV_PIX_FMT_FLAG_ALPHA && !(draw->flags & FF_DRAW_PROCESS_ALPHA));

    clip_interval(dst_w, &x0, &mask_w, &xm0);
    clip_interval(dst_h, &y0, &mask_h, &ym0);
    mask += ym0 * mask_linesize;
    if (mask_w <= 0 || mask_h <= 0 || !color->rgba[3])
        return;
    if (draw->desc->comp[0].depth <= 8) {
        /* alpha is in the [ 0 ; 0x10203 ] range,
           alpha * mask is in the [ 0 ; 0x1010101 - 4 ] range */
        alpha = (0x10307 * color->rgba[3] + 0x3) >> 8;
    } else {
        alpha = (0x101 * color->rgba[3] + 0x2) >> 8;
    }
    nb_planes = draw->nb_planes - !!(draw->desc->flags & AV_PIX_FMT_FLAG_ALPHA && !(draw->flags & FF_DRAW_PROCESS_ALPHA));
    nb_planes += !nb_planes;
    for (unsigned plane = 0; plane < nb_planes; plane++) {
        uint8_t *p0 = pointer_at(draw, dst, dst_linesize, plane, x0, y0);
        w_sub = mask_w;
        h_sub = mask_h;
        x_sub = x0;
        y_sub = y0;
        subsampling_bounds(draw->hsub[plane], &x_sub, &w_sub, &left, &right);
        subsampling_bounds(draw->vsub[plane], &y_sub, &h_sub, &top, &bottom);
        for (unsigned comp = 0; comp < nb_comp; comp++) {
            const int depth = draw->desc->comp[comp].depth;
            const int offset = draw->desc->comp[comp].offset;
            const int index = offset / ((depth + 7) / 8);

            if (draw->desc->comp[comp].plane != plane)
                continue;
            p = p0 + offset;
            m = mask;
            if (top) {
                if (depth <= 8) {
                    blend_line_hv(p, draw->pixelstep[plane],
                                  color->comp[plane].u8[index], alpha,
                                  m, mask_linesize, l2depth, w_sub,
                                  draw->hsub[plane], draw->vsub[plane],
                                  xm0, left, right, top);
                } else {
                    blend_line_hv16(p, draw->pixelstep[plane],
                                    color->comp[plane].u16[index], alpha,
                                    m, mask_linesize, l2depth, w_sub,
                                    draw->hsub[plane], draw->vsub[plane],
                                    xm0, left, right, top);
                }
                p += dst_linesize[plane];
                m += top * mask_linesize;
            }
            if (depth <= 8) {
                int64_t t8_start = av_gettime_relative();
                for (int y = 0; y < h_sub; y++) {
                    blend_line_hv(p, draw->pixelstep[plane],
                                  color->comp[plane].u8[index], alpha,
                                  m, mask_linesize, l2depth, w_sub,
                                  draw->hsub[plane], draw->vsub[plane],
                                  xm0, left, right, 1 << draw->vsub[plane]);
                    p += dst_linesize[plane];
                    m += mask_linesize << draw->vsub[plane];
                }
                s_blend8_time += av_gettime_relative() - t8_start;
                s_blend8_count++;
            } else {
                int simd_used = 0;
                int64_t t16_start = av_gettime_relative();
#if ARCH_X86 && HAVE_AVX2_INLINE
                if (l2depth == 3 && depth < 16 &&
                    draw->pixelstep[plane] == 2 &&
                    draw->vsub[plane] == 0) {
                    unsigned cpu = av_get_cpu_flags();
#if HAVE_AVX512_INLINE
#define BLEND16_LINE_LUMA(fn)                                               \
    do {                                                                     \
        for (int y = 0; y < h_sub; y++) {                                   \
            fn((uint16_t *)p, src16, alpha, m, xm0, w_sub);                 \
            p += dst_linesize[plane];                                        \
            m += mask_linesize;                                              \
        }                                                                    \
    } while (0)
#define BLEND16_LINE_CHROMA(fn)                                              \
    do {                                                                     \
        for (int y = 0; y < h_sub; y++) {                                   \
            uint16_t *dstp = (uint16_t *)p;                                  \
            const uint8_t *mp = m;                                           \
            int xm = xm0;                                                    \
            if (left) {                                                      \
                unsigned a = (mp[xm] * alpha) >> 1;                         \
                unsigned v = dstp[0];                                        \
                dstp[0] = ((0x10001 - a) * v + a * src16) >> 16;            \
                dstp++; xm++;                                                \
            }                                                                \
            fn(dstp, src16, alpha, mp, xm, w_sub);                          \
            if (right) {                                                     \
                dstp += w_sub; xm += w_sub * 2;                             \
                unsigned a = (mp[xm] * alpha) >> 1;                         \
                unsigned v = dstp[0];                                        \
                dstp[0] = ((0x10001 - a) * v + a * src16) >> 16;            \
            }                                                                \
            p += dst_linesize[plane];                                        \
            m += mask_linesize;                                              \
        }                                                                    \
    } while (0)
                    unsigned src16 = color->comp[plane].u16[index];
                    if (cpu & AV_CPU_FLAG_AVX512) {
                        simd_used = 1;
                        if (draw->hsub[plane] == 0)
                            BLEND16_LINE_LUMA(blend_line16_luma_avx512);
                        else if (draw->hsub[plane] == 1)
                            BLEND16_LINE_CHROMA(blend_line16_chroma422_avx512);
                        else
                            simd_used = 0;
                    }
                    if (!simd_used && (cpu & AV_CPU_FLAG_AVX2)) {
#else
                    unsigned src16 = color->comp[plane].u16[index];
                    if (cpu & AV_CPU_FLAG_AVX2) {
#endif
                        if (draw->hsub[plane] == 0) {
                            simd_used = 1;
                            BLEND16_LINE_LUMA(blend_line16_luma_avx2);
                        } else if (draw->hsub[plane] == 1) {
                            simd_used = 1;
                            BLEND16_LINE_CHROMA(blend_line16_chroma422_avx2);
                        }
                    }
#if HAVE_AVX512_INLINE
#undef BLEND16_LINE_LUMA
#undef BLEND16_LINE_CHROMA
#endif
                }
#endif /* ARCH_X86 && HAVE_AVX2_INLINE */
                if (simd_used) {
                    s_blend16_simd_time += av_gettime_relative() - t16_start;
                    s_blend16_simd_count++;
                }
                if (!simd_used) {
                    for (int y = 0; y < h_sub; y++) {
                        blend_line_hv16(p, draw->pixelstep[plane],
                                        color->comp[plane].u16[index], alpha,
                                        m, mask_linesize, l2depth, w_sub,
                                        draw->hsub[plane], draw->vsub[plane],
                                        xm0, left, right, 1 << draw->vsub[plane]);
                        p += dst_linesize[plane];
                        m += mask_linesize << draw->vsub[plane];
                    }
                    s_blend16_time += av_gettime_relative() - t16_start;
                    s_blend16_count++;
                }
            }
            if (bottom) {
                if (depth <= 8) {
                    blend_line_hv(p, draw->pixelstep[plane],
                                  color->comp[plane].u8[index], alpha,
                                  m, mask_linesize, l2depth, w_sub,
                                  draw->hsub[plane], draw->vsub[plane],
                                  xm0, left, right, bottom);
                } else {
                    blend_line_hv16(p, draw->pixelstep[plane],
                                    color->comp[plane].u16[index], alpha,
                                    m, mask_linesize, l2depth, w_sub,
                                    draw->hsub[plane], draw->vsub[plane],
                                    xm0, left, right, bottom);
                }
            }
        }
    }

    // Log blend timing stats periodically (every 1000 calls) at DEBUG level
    s_log_counter++;
    if (s_log_counter >= 1000) {
        av_log(NULL, AV_LOG_DEBUG,
               "TIMING ff_blend_mask (per 1000 calls): "
               "blend8=%"PRId64"us(%d) blend16_scalar=%"PRId64"us(%d) blend16_simd=%"PRId64"us(%d)\n",
               s_blend8_time, s_blend8_count,
               s_blend16_time, s_blend16_count,
               s_blend16_simd_time, s_blend16_simd_count);
        s_blend8_time = s_blend16_time = s_blend16_simd_time = 0;
        s_blend8_count = s_blend16_count = s_blend16_simd_count = 0;
        s_log_counter = 0;
    }
}

int ff_draw_round_to_sub(FFDrawContext *draw, int sub_dir, int round_dir,
                         int value)
{
    unsigned shift = sub_dir ? draw->vsub_max : draw->hsub_max;

    if (!shift)
        return value;
    if (round_dir >= 0)
        value += round_dir ? (1 << shift) - 1 : 1 << (shift - 1);
    return (value >> shift) << shift;
}

AVFilterFormats *ff_draw_supported_pixel_formats(unsigned flags)
{
    FFDrawContext draw;
    AVFilterFormats *fmts = NULL;
    int ret;

    for (enum AVPixelFormat i = 0; av_pix_fmt_desc_get(i); i++)
        if (ff_draw_init(&draw, i, flags) >= 0 &&
            (ret = ff_add_format(&fmts, i)) < 0)
            return NULL;
    return fmts;
}
