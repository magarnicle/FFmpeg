;*****************************************************************************
;* x86-optimized functions for overlay filter
;*
;* Copyright (C) 2018 Paul B Mahol
;* Copyright (C) 2018 Henrik Gramner
;*
;* This file is part of FFmpeg.
;*
;* FFmpeg is free software; you can redistribute it and/or
;* modify it under the terms of the GNU Lesser General Public
;* License as published by the Free Software Foundation; either
;* version 2.1 of the License, or (at your option) any later version.
;*
;* FFmpeg is distributed in the hope that it will be useful,
;* but WITHOUT ANY WARRANTY; without even the implied warranty of
;* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
;* Lesser General Public License for more details.
;*
;* You should have received a copy of the GNU Lesser General Public
;* License along with FFmpeg; if not, write to the Free Software
;* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
;*****************************************************************************

%include "libavutil/x86/x86util.asm"

SECTION_RODATA

pb_1:     times 16 db 1
pw_128:   times  8 dw 128
pw_255:   times  8 dw 255
pw_257:   times  8 dw 257
pd_512:   times  4 dd 512
pd_1023:  times  4 dd 1023

SECTION .text

INIT_XMM sse4
cglobal overlay_row_44, 5, 7, 6, 0, dst, da, s, a, w, r, x
    xor          xq, xq
    movsxdifnidn wq, wd
    mov          rq, wq
    and          rq, mmsize/2 - 1
    cmp          wq, mmsize/2
    jl .end
    sub          wq, rq
    mova         m3, [pw_255]
    mova         m4, [pw_128]
    mova         m5, [pw_257]
    .loop:
        pmovzxbw    m0, [sq+xq]
        pmovzxbw    m2, [aq+xq]
        pmovzxbw    m1, [dstq+xq]
        pmullw      m0, m2
        pxor        m2, m3
        pmullw      m1, m2
        paddw       m0, m4
        paddw       m0, m1
        pmulhuw     m0, m5
        packuswb    m0, m0
        movq [dstq+xq], m0
        add         xq, mmsize/2
        cmp         xq, wq
        jl .loop

    .end:
    mov    eax, xd
    RET

INIT_XMM sse4
cglobal overlay_row_22, 5, 7, 6, 0, dst, da, s, a, w, r, x
    xor          xq, xq
    movsxdifnidn wq, wd
    sub          wq, 1
    mov          rq, wq
    and          rq, mmsize/2 - 1
    cmp          wq, mmsize/2
    jl .end
    sub          wq, rq
    mova         m3, [pw_255]
    mova         m4, [pw_128]
    mova         m5, [pw_257]
    .loop:
        pmovzxbw    m0, [sq+xq]
        movu        m1, [aq+2*xq]
        pandn       m2, m3, m1
        psllw       m1, 8
        pavgw       m2, m1
        pavgw       m2, m1
        psrlw       m2, 8
        pmovzxbw    m1, [dstq+xq]
        pmullw      m0, m2
        pxor        m2, m3
        pmullw      m1, m2
        paddw       m0, m4
        paddw       m0, m1
        pmulhuw     m0, m5
        packuswb    m0, m0
        movq [dstq+xq], m0
        add         xq, mmsize/2
        cmp         xq, wq
        jl .loop

    .end:
    mov    eax, xd
    RET

; 10-bit 4:4:4 overlay blend (straight alpha, no main alpha)
; Computes: dst = (dst * (1023 - alpha) + src * alpha + 512) >> 10
; Processes 4 pixels per iteration using 32-bit intermediates
INIT_XMM sse4
cglobal overlay_row_44_10, 5, 7, 8, 0, dst, da, s, a, w, r, x
    xor          xq, xq
    movsxdifnidn wq, wd
    mov          rq, wq
    and          rq, 3                  ; remainder
    cmp          wq, 4
    jl .end
    sub          wq, rq
    mova         m6, [pd_1023]
    mova         m7, [pd_512]
    .loop:
        ; Load 4 pixels (16-bit each) and zero-extend to 32-bit
        movq        m0, [sq + xq*2]      ; src (4 x 16-bit)
        movq        m1, [dstq + xq*2]    ; dst
        movq        m2, [aq + xq*2]      ; alpha

        pmovzxwd    m0, m0               ; src -> 4 x 32-bit
        pmovzxwd    m1, m1               ; dst -> 4 x 32-bit
        pmovzxwd    m2, m2               ; alpha -> 4 x 32-bit

        ; m3 = 1023 - alpha
        mova        m3, m6
        psubd       m3, m2

        ; m1 = dst * (1023 - alpha)
        pmulld      m1, m3

        ; m0 = src * alpha
        pmulld      m0, m2

        ; m0 = dst*(1023-alpha) + src*alpha + 512
        paddd       m0, m1
        paddd       m0, m7

        ; m0 = result >> 10
        psrld       m0, 10

        ; Pack back to 16-bit and store
        packusdw    m0, m0
        movq  [dstq + xq*2], m0

        add         xq, 4
        cmp         xq, wq
        jl .loop

    .end:
    mov    eax, xd
    RET

; AVX2 version - processes 8 pixels per iteration
INIT_YMM avx2
cglobal overlay_row_44_10, 5, 7, 8, 0, dst, da, s, a, w, r, x
    xor          xq, xq
    movsxdifnidn wq, wd
    mov          rq, wq
    and          rq, 7                  ; remainder
    cmp          wq, 8
    jl .end
    sub          wq, rq
    vpbroadcastd m6, [pd_1023]
    vpbroadcastd m7, [pd_512]
    .loop:
        ; Load 8 pixels (16-bit each) and zero-extend to 32-bit
        vpmovzxwd   m0, [sq + xq*2]      ; src -> 8 x 32-bit
        vpmovzxwd   m1, [dstq + xq*2]    ; dst -> 8 x 32-bit
        vpmovzxwd   m2, [aq + xq*2]      ; alpha -> 8 x 32-bit

        ; m3 = 1023 - alpha
        vpsubd      m3, m6, m2

        ; m1 = dst * (1023 - alpha)
        vpmulld     m1, m1, m3

        ; m0 = src * alpha
        vpmulld     m0, m0, m2

        ; m0 = dst*(1023-alpha) + src*alpha + 512
        vpaddd      m0, m0, m1
        vpaddd      m0, m0, m7

        ; m0 = result >> 10
        vpsrld      m0, m0, 10

        ; Pack back to 16-bit: need to pack 8x32 -> 8x16
        ; packusdw in AVX2 works within 128-bit lanes, so we need to permute
        vextracti128 xm1, m0, 1          ; high 4 dwords
        packusdw    xm0, xm1             ; pack to 8 x 16-bit
        movu  [dstq + xq*2], xm0

        add         xq, 8
        cmp         xq, wq
        jl .loop

    .end:
    mov    eax, xd
    RET

INIT_XMM sse4
cglobal overlay_row_20, 6, 7, 7, 0, dst, da, s, a, w, r, x
    mov         daq, aq
    add         daq, rmp
    xor          xq, xq
    movsxdifnidn wq, wd
    sub          wq, 1
    mov          rq, wq
    and          rq, mmsize/2 - 1
    cmp          wq, mmsize/2
    jl .end
    sub          wq, rq
    mova         m3, [pw_255]
    mova         m4, [pw_128]
    mova         m5, [pw_257]
    mova         m6, [pb_1]
    .loop:
        pmovzxbw    m0, [sq+xq]
        movu        m2, [aq+2*xq]
        movu        m1, [daq+2*xq]
        pmaddubsw   m2, m6
        pmaddubsw   m1, m6
        paddw       m2, m1
        psrlw       m2, 2
        pmovzxbw    m1, [dstq+xq]
        pmullw      m0, m2
        pxor        m2, m3
        pmullw      m1, m2
        paddw       m0, m4
        paddw       m0, m1
        pmulhuw     m0, m5
        packuswb    m0, m0
        movq [dstq+xq], m0
        add         xq, mmsize/2
        cmp         xq, wq
        jl .loop

    .end:
    mov    eax, xd
    RET
