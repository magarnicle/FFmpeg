/*
 * OP-47 Teletext Subtitle Extractor
 * Extracts WST teletext subtitles from MXF files containing OP-47/SDP VANC data
 *
 * Usage: op47_extract input.mxf output.srt [page]
 *        page defaults to 801
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libavcodec/smpte_436m.h"

#define MAX_SUBS 10000
#define MAX_TEXT_LEN 512

struct subtitle_entry {
    int64_t start_pts;
    int64_t end_pts;
    char text[MAX_TEXT_LEN];
};

static struct subtitle_entry subs[MAX_SUBS];
static int sub_count = 0;
static int target_page = 801;
static AVRational time_base;

/* Hamming 8/4 decode - returns 4-bit value or -1 on error */
static int hamming84_decode(uint8_t b)
{
    static const int8_t hamming84_table[256] = {
        0x01, 0xff, 0x01, 0x01, 0xff, 0x00, 0x01, 0xff,
        0xff, 0x02, 0x01, 0xff, 0x0a, 0xff, 0xff, 0x07,
        0xff, 0x00, 0x01, 0xff, 0x00, 0x00, 0xff, 0x00,
        0x06, 0xff, 0xff, 0x0b, 0xff, 0x00, 0x03, 0xff,
        0xff, 0x0c, 0x01, 0xff, 0x04, 0xff, 0xff, 0x07,
        0x06, 0xff, 0xff, 0x07, 0xff, 0x07, 0x07, 0x07,
        0x06, 0xff, 0xff, 0x05, 0xff, 0x00, 0x0d, 0xff,
        0x06, 0x06, 0x06, 0xff, 0x06, 0xff, 0xff, 0x07,
        0xff, 0x02, 0x01, 0xff, 0x04, 0xff, 0xff, 0x09,
        0x02, 0x02, 0xff, 0x02, 0xff, 0x02, 0x03, 0xff,
        0x08, 0xff, 0xff, 0x05, 0xff, 0x00, 0x03, 0xff,
        0xff, 0x02, 0x03, 0xff, 0x03, 0xff, 0x03, 0x03,
        0x04, 0xff, 0xff, 0x05, 0x04, 0x04, 0x04, 0xff,
        0xff, 0x02, 0x0f, 0xff, 0x04, 0xff, 0xff, 0x07,
        0xff, 0x05, 0x05, 0x05, 0x04, 0xff, 0xff, 0x05,
        0x06, 0xff, 0xff, 0x05, 0xff, 0x0e, 0x03, 0xff,
        0xff, 0x0c, 0x01, 0xff, 0x0a, 0xff, 0xff, 0x09,
        0x0a, 0xff, 0xff, 0x0b, 0x0a, 0x0a, 0x0a, 0xff,
        0x08, 0xff, 0xff, 0x0b, 0xff, 0x00, 0x0d, 0xff,
        0xff, 0x0b, 0x0b, 0x0b, 0x0a, 0xff, 0xff, 0x0b,
        0x0c, 0x0c, 0xff, 0x0c, 0xff, 0x0c, 0x0d, 0xff,
        0xff, 0x0c, 0x0f, 0xff, 0x0a, 0xff, 0xff, 0x07,
        0xff, 0x0c, 0x0d, 0xff, 0x0d, 0xff, 0x0d, 0x0d,
        0x06, 0xff, 0xff, 0x0b, 0xff, 0x0e, 0x0d, 0xff,
        0x08, 0xff, 0xff, 0x09, 0xff, 0x09, 0x09, 0x09,
        0xff, 0x02, 0x0f, 0xff, 0x0a, 0xff, 0xff, 0x09,
        0x08, 0x08, 0x08, 0xff, 0x08, 0xff, 0xff, 0x09,
        0x08, 0xff, 0xff, 0x0b, 0xff, 0x0e, 0x03, 0xff,
        0xff, 0x0c, 0x0f, 0xff, 0x04, 0xff, 0xff, 0x09,
        0x0f, 0xff, 0x0f, 0x0f, 0xff, 0x0e, 0x0f, 0xff,
        0x08, 0xff, 0xff, 0x05, 0xff, 0x0e, 0x0d, 0xff,
        0xff, 0x0e, 0x0f, 0xff, 0x0e, 0x0e, 0xff, 0x0e,
    };
    int v = hamming84_table[b];
    return (v == 0xff) ? -1 : v;
}

/* Strip parity bit */
static int parity_strip(uint8_t b)
{
    return b & 0x7f;
}

/* Page state */
static char row_text[26][64];
static int row_valid[26];
static int current_page = 0;
static int on_target_page = 0;

/* Subtitle state */
static char current_text[MAX_TEXT_LEN] = {0};
static char last_text[MAX_TEXT_LEN] = {0};
static int64_t current_start_pts = AV_NOPTS_VALUE;
static int have_subtitle = 0;

static void clear_page(void)
{
    memset(row_text, 0, sizeof(row_text));
    memset(row_valid, 0, sizeof(row_valid));
}

static void flush_subtitle(int64_t end_pts)
{
    if (have_subtitle && last_text[0] && sub_count < MAX_SUBS) {
        subs[sub_count].start_pts = current_start_pts;
        subs[sub_count].end_pts = end_pts;
        snprintf(subs[sub_count].text, MAX_TEXT_LEN, "%s", last_text);
        sub_count++;
    }
    have_subtitle = 0;
    last_text[0] = '\0';
}

static void build_subtitle_text(int64_t pts)
{
    char combined[MAX_TEXT_LEN] = {0};
    int pos = 0;

    for (int row = 1; row <= 25; row++) {
        if (row_valid[row] && row_text[row][0]) {
            char *text = row_text[row];

            /* Trim trailing spaces */
            int len = strlen(text);
            while (len > 0 && text[len-1] == ' ')
                text[--len] = '\0';

            /* Trim leading spaces */
            char *start = text;
            while (*start == ' ') start++;

            if (*start) {
                if (pos > 0 && pos < MAX_TEXT_LEN - 2) {
                    combined[pos++] = '\n';
                }
                int slen = strlen(start);
                if (pos + slen < MAX_TEXT_LEN - 1) {
                    memcpy(combined + pos, start, slen);
                    pos += slen;
                }
            }
        }
    }
    combined[pos] = '\0';

    /* Check if text changed */
    if (strcmp(combined, current_text) != 0) {
        /* Text changed - flush previous and start new */
        if (have_subtitle && last_text[0]) {
            flush_subtitle(pts);
        }

        snprintf(current_text, MAX_TEXT_LEN, "%s", combined);

        if (combined[0]) {
            snprintf(last_text, MAX_TEXT_LEN, "%s", combined);
            current_start_pts = pts;
            have_subtitle = 1;
        }
    }
}

/* Process OP-47 SDP packet */
static void process_sdp_packet(const uint8_t *data, int len, int64_t pts)
{
    if (len < 9)
        return;

    /* Check OP-47 identifier: 0x5115 */
    if (data[0] != 0x51 || data[1] != 0x15)
        return;

    uint8_t format_code = data[3];
    if (format_code != 0x02)  /* WSS Teletext */
        return;

    /* Parse line pointers (bytes 4-8) */
    int line_present[5] = {0};

    for (int i = 0; i < 5; i++) {
        uint8_t ptr = data[4 + i];
        if (ptr != 0) {
            line_present[i] = 1;
        }
    }

    /* Parse structure B blocks (45 bytes each) */
    int offset = 9;
    for (int i = 0; i < 5 && offset + 45 <= len; i++) {
        if (!line_present[i])
            continue;

        const uint8_t *block = &data[offset];
        offset += 45;

        /* OP-47 Structure B: clock run-in (0x55 0x55), framing (0x27), then 42 bytes data */
        if (block[0] != 0x55 || block[1] != 0x55 || block[2] != 0x27)
            continue;

        const uint8_t *teletext = &block[3];

        int mag_addr = hamming84_decode(teletext[0]);
        int pkt_addr_lo = hamming84_decode(teletext[1]);

        if (mag_addr < 0 || pkt_addr_lo < 0)
            continue;

        int magazine = mag_addr & 0x07;
        if (magazine == 0) magazine = 8;

        int packet_num = ((mag_addr >> 3) & 0x01) | (pkt_addr_lo << 1);

        /* Packet 0 = page header */
        if (packet_num == 0) {
            int page_units = hamming84_decode(teletext[2]);
            int page_tens = hamming84_decode(teletext[3]);

            if (page_units >= 0 && page_tens >= 0) {
                int page = magazine * 100 + page_tens * 10 + page_units;
                current_page = page;

                if (page == target_page) {
                    if (!on_target_page) {
                        /* Entering target page */
                        on_target_page = 1;
                        clear_page();
                    }

                    /* Check C4 (erase page) - bit 3 of control byte */
                    int c_bits = hamming84_decode(teletext[5]);
                    if (c_bits >= 0 && (c_bits & 0x08)) {
                        /* Erase flag - clear and flush */
                        build_subtitle_text(pts);
                        clear_page();
                    }
                } else if (on_target_page) {
                    /* Left target page */
                    build_subtitle_text(pts);
                    on_target_page = 0;
                }
            }
        }
        /* Packets 1-25 = display rows */
        else if (packet_num >= 1 && packet_num <= 25 && on_target_page) {
            char text[64] = {0};
            int textpos = 0;

            for (int j = 2; j < 42 && textpos < 63; j++) {
                uint8_t c = parity_strip(teletext[j]);

                /* Skip most control codes */
                if (c < 0x20) {
                    continue;
                }

                /* Map printable characters */
                if (c >= 0x20 && c < 0x7f) {
                    text[textpos++] = c;
                } else {
                    text[textpos++] = ' ';
                }
            }
            text[textpos] = '\0';

            snprintf(row_text[packet_num], sizeof(row_text[packet_num]), "%s", text);
            row_valid[packet_num] = 1;

            /* Build subtitle after each row update */
            build_subtitle_text(pts);
        }
    }
}

/* Format timecode for SRT */
static void format_srt_time(int64_t pts, AVRational tb, char *buf, size_t bufsize)
{
    double seconds = pts * av_q2d(tb);
    if (seconds < 0) seconds = 0;

    int hours = (int)(seconds / 3600);
    int minutes = (int)((seconds - hours * 3600) / 60);
    int secs = (int)(seconds - hours * 3600 - minutes * 60);
    int millis = (int)((seconds - (int)seconds) * 1000);

    snprintf(buf, bufsize, "%02d:%02d:%02d,%03d", hours, minutes, secs, millis);
}

/* Merge short subtitles and write SRT file */
static int write_srt(const char *filename)
{
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "Cannot open output file: %s\n", filename);
        return -1;
    }

    int output_count = 0;
    int i = 0;

    while (i < sub_count) {
        int64_t start = subs[i].start_pts;
        int64_t end = subs[i].end_pts;
        char *text = subs[i].text;

        /* Merge with next subtitle if:
         * - Duration is very short (< 200ms)
         * - Next subtitle starts immediately or overlaps
         * - Next subtitle text is a superset (starts with same text)
         */
        while (i + 1 < sub_count) {
            double duration = (end - start) * av_q2d(time_base);
            int64_t gap = subs[i+1].start_pts - end;

            if (duration < 0.2 && gap <= 2) {
                /* Very short - merge with next */
                end = subs[i+1].end_pts;
                text = subs[i+1].text;
                i++;
            } else if (gap <= 1 && strstr(subs[i+1].text, text) == subs[i+1].text) {
                /* Next is superset of current - use next */
                end = subs[i+1].end_pts;
                text = subs[i+1].text;
                i++;
            } else {
                break;
            }
        }

        /* Skip very short final subtitles */
        double final_duration = (end - start) * av_q2d(time_base);
        if (final_duration >= 0.1) {
            char start_time[32], end_time[32];
            format_srt_time(start, time_base, start_time, sizeof(start_time));
            format_srt_time(end, time_base, end_time, sizeof(end_time));

            output_count++;
            fprintf(f, "%d\n%s --> %s\n%s\n\n", output_count, start_time, end_time, text);
        }

        i++;
    }

    fclose(f);
    fprintf(stderr, "Merged %d raw subtitles to %d output subtitles\n", sub_count, output_count);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s input.mxf output.srt [page]\n", argv[0]);
        fprintf(stderr, "       page defaults to 801\n");
        return 1;
    }

    const char *input_file = argv[1];
    const char *output_file = argv[2];

    if (argc > 3) {
        target_page = atoi(argv[3]);
    }

    fprintf(stderr, "Extracting teletext page %d from %s\n", target_page, input_file);

    /* Open input file */
    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, input_file, NULL, NULL) < 0) {
        fprintf(stderr, "Cannot open input file: %s\n", input_file);
        return 1;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Cannot find stream info\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    /* Find the data stream (SMPTE 436M ancillary) */
    int data_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_DATA &&
            fmt_ctx->streams[i]->codecpar->codec_id == AV_CODEC_ID_SMPTE_436M_ANC) {
            data_stream_idx = i;
            time_base = fmt_ctx->streams[i]->time_base;
            break;
        }
    }

    if (data_stream_idx < 0) {
        fprintf(stderr, "No SMPTE 436M data stream found\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    fprintf(stderr, "Found SMPTE 436M stream at index %d\n", data_stream_idx);

    /* Read packets */
    AVPacket *pkt = av_packet_alloc();
    int64_t last_pts = 0;
    int packet_count = 0;
    int sdp_count = 0;

    clear_page();

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == data_stream_idx) {
            last_pts = pkt->pts;
            packet_count++;

            AVSmpte436mAncIterator iter;
            if (av_smpte_436m_anc_iter_init(&iter, pkt->data, pkt->size) == 0) {
                AVSmpte436mCodedAnc coded_anc;

                while (av_smpte_436m_anc_iter_next(&iter, &coded_anc) == 0) {
                    AVSmpte291mAnc8bit anc;
                    if (av_smpte_291m_anc_8bit_decode(&anc,
                                                      coded_anc.payload_sample_coding,
                                                      coded_anc.payload_sample_count,
                                                      coded_anc.payload,
                                                      NULL) == 0) {
                        /* OP-47 SDP: DID=0x43, SDID=0x02 */
                        if (anc.did == 0x43 && anc.sdid_or_dbn == 0x02) {
                            sdp_count++;
                            process_sdp_packet(anc.payload, anc.data_count, pkt->pts);
                        }
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

    /* Flush final subtitle */
    if (have_subtitle) {
        flush_subtitle(last_pts + 3 * time_base.den / time_base.num);
    }

    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);

    fprintf(stderr, "Processed %d data packets, %d SDP packets, found %d subtitles\n",
            packet_count, sdp_count, sub_count);

    /* Write output */
    if (sub_count > 0) {
        if (write_srt(output_file) == 0) {
            fprintf(stderr, "Written %s\n", output_file);
        }
    } else {
        fprintf(stderr, "No subtitles found on page %d\n", target_page);
        return 1;
    }

    return 0;
}
