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

#ifndef FFTOOLS_FFMPEG_SUPER_CONCAT_H
#define FFTOOLS_FFMPEG_SUPER_CONCAT_H

#include "libavutil/dict.h"

typedef struct SCInput {
    char *url;
    AVDictionary *options;  /* per-input options: ss, t, etc. */
} SCInput;

typedef struct SCSegment {
    SCInput *inputs;
    int nb_inputs;
    char *filter;           /* filtergraph string, e.g. "[0:v][1:v]overlay[vid]" */
    char **maps;            /* output pad names, e.g. ["[vid]", "[0:a]"] */
    int nb_maps;
} SCSegment;

typedef struct SCPlaylist {
    SCSegment *segments;
    int nb_segments;
} SCPlaylist;

/**
 * Main entry point for super concat mode.
 *
 * @param playlist_file  path to JSON playlist file
 * @param output_url     output file path
 * @param argc           remaining argc (for output codec options)
 * @param argv           remaining argv (for output codec options)
 * @return 0 on success, negative AVERROR on failure
 */
int super_concat_main(const char *playlist_file, const char *output_url,
                      int argc, char **argv);

#endif /* FFTOOLS_FFMPEG_SUPER_CONCAT_H */
