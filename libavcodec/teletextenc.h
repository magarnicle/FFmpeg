/*
 * Teletext encoder
 * Copyright (c) 2024 FFmpeg
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

#ifndef AVCODEC_TELETEXTENC_H
#define AVCODEC_TELETEXTENC_H

#include <stdint.h>

/**
 * Teletext constants
 */
#define TELETEXT_ROWS           24
#define TELETEXT_COLS           40
#define TELETEXT_LINE_SIZE      42   /* Teletext line payload size */
#define TELETEXT_DATA_UNIT_SIZE 46   /* Full data unit with header */

/**
 * Data unit IDs for teletext
 */
#define TELETEXT_DATA_UNIT_EBU_TELETEXT_NON_SUBTITLE  0x02
#define TELETEXT_DATA_UNIT_EBU_TELETEXT_SUBTITLE      0x03

/**
 * Teletext control codes (spacing attributes)
 */
#define TELETEXT_ALPHA_BLACK     0x00
#define TELETEXT_ALPHA_RED       0x01
#define TELETEXT_ALPHA_GREEN     0x02
#define TELETEXT_ALPHA_YELLOW    0x03
#define TELETEXT_ALPHA_BLUE      0x04
#define TELETEXT_ALPHA_MAGENTA   0x05
#define TELETEXT_ALPHA_CYAN      0x06
#define TELETEXT_ALPHA_WHITE     0x07
#define TELETEXT_FLASH           0x08
#define TELETEXT_STEADY          0x09
#define TELETEXT_END_BOX         0x0A
#define TELETEXT_START_BOX       0x0B
#define TELETEXT_NORMAL_HEIGHT   0x0C
#define TELETEXT_DOUBLE_HEIGHT   0x0D
#define TELETEXT_DOUBLE_WIDTH    0x0E
#define TELETEXT_DOUBLE_SIZE     0x0F
#define TELETEXT_MOSAIC_BLACK    0x10
#define TELETEXT_MOSAIC_RED      0x11
#define TELETEXT_MOSAIC_GREEN    0x12
#define TELETEXT_MOSAIC_YELLOW   0x13
#define TELETEXT_MOSAIC_BLUE     0x14
#define TELETEXT_MOSAIC_MAGENTA  0x15
#define TELETEXT_MOSAIC_CYAN     0x16
#define TELETEXT_MOSAIC_WHITE    0x17
#define TELETEXT_CONCEAL         0x18
#define TELETEXT_CONTIGUOUS_MOSAIC  0x19
#define TELETEXT_SEPARATED_MOSAIC   0x1A
#define TELETEXT_ESC             0x1B
#define TELETEXT_BLACK_BACKGROUND   0x1C
#define TELETEXT_NEW_BACKGROUND     0x1D
#define TELETEXT_HOLD_MOSAIC        0x1E
#define TELETEXT_RELEASE_MOSAIC     0x1F

/**
 * Hamming 8/4 encoding table
 */
extern const uint8_t ff_teletext_ham84_encode[16];

/**
 * Encode a nibble using Hamming 8/4
 */
uint8_t ff_teletext_ham84(uint8_t nibble);

/**
 * Add odd parity to a 7-bit character
 */
uint8_t ff_teletext_odd_parity(uint8_t c);

/**
 * Build a teletext data unit (46 bytes)
 *
 * @param out        Output buffer (must be at least 46 bytes)
 * @param data_unit_id  Data unit ID (0x02 or 0x03)
 * @param field_parity  Field parity (0 = even/first field, 1 = odd/second field)
 * @param line_offset   Line offset within field
 * @param magazine      Magazine number (1-8, will be encoded as 0-7)
 * @param packet_number Row/packet number (0-31)
 * @param data          42 bytes of teletext line data (with parity already applied)
 * @return 0 on success, negative on error
 */
int ff_teletext_build_data_unit(uint8_t *out, int data_unit_id,
                                int field_parity, int line_offset,
                                int magazine, int packet_number,
                                const uint8_t *data);

#endif /* AVCODEC_TELETEXTENC_H */
