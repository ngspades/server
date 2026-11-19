// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "text.h"

/* Unicode code points of CP437 0x80..0xFF */
static const uint16_t cp437_high[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE,
    0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6,
    0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA,
    0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502,
    0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514,
    0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550,
    0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C,
    0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320,
    0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

size_t p10_cp437_to_utf8(const uint8_t* in, size_t in_len, char* out, size_t out_cap)
{
    size_t n = 0;
    if (out_cap == 0)
        return 0;
    for (size_t i = 0; i < in_len && in[i] != 0; i++) {
        uint32_t cp = in[i] < 0x80 ? in[i] : cp437_high[in[i] - 0x80];
        uint8_t  enc[3];
        size_t   len;
        if (cp < 0x80) {
            enc[0] = (uint8_t) cp;
            len    = 1;
        } else if (cp < 0x800) {
            enc[0] = (uint8_t) (0xC0 | (cp >> 6));
            enc[1] = (uint8_t) (0x80 | (cp & 0x3F));
            len    = 2;
        } else {
            enc[0] = (uint8_t) (0xE0 | (cp >> 12));
            enc[1] = (uint8_t) (0x80 | ((cp >> 6) & 0x3F));
            enc[2] = (uint8_t) (0x80 | (cp & 0x3F));
            len    = 3;
        }
        /* cut on a character boundary, never in the middle of one */
        if (n + len > out_cap - 1)
            break;
        for (size_t k = 0; k < len; k++)
            out[n++] = (char) enc[k];
    }
    out[n] = 0;
    return n;
}

static uint8_t cp437_of(uint32_t cp)
{
    if (cp < 0x80)
        return cp == 0 ? (uint8_t) '?' : (uint8_t) cp;
    for (int k = 0; k < 128; k++) {
        if (cp437_high[k] == cp)
            return (uint8_t) (0x80 + k);
    }
    return (uint8_t) '?';
}

size_t p10_utf8_to_cp437(p10_bytes in, char* out, size_t out_cap)
{
    size_t n = 0, i = 0;
    if (out_cap == 0)
        return 0;
    while (i < in.len && n < out_cap - 1) {
        uint8_t  c = in.data[i];
        uint32_t cp;
        size_t   len;
        if (c < 0x80) {
            cp  = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp  = c & 0x1Fu;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp  = c & 0x0Fu;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp  = c & 0x07u;
            len = 4;
        } else {
            out[n++] = '?';
            i++;
            continue;
        }
        size_t k = 1;
        for (; k < len && i + k < in.len && (in.data[i + k] & 0xC0) == 0x80; k++)
            cp = (cp << 6) | (in.data[i + k] & 0x3Fu);
        if (k < len) {
            /* truncated or broken sequence: one '?' for its lead byte */
            out[n++] = '?';
            i++;
            continue;
        }
        out[n++] = (char) cp437_of(cp);
        i += len;
    }
    out[n] = 0;
    return n;
}
