// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Text conversion at the protocol boundary (docs/protocol-1.0.md 12.2.1).
 *
 * 0.75 text (chat, notices, the client's chat line and font) is CP437 bytes;
 * 1.0 text is UTF-8. Both programs keep CP437 inside, because their gameplay
 * code (the 0.75 message handler, the client's bitmap font) was written for
 * it, and convert only where a 1.0 packet is read or written.
 *
 * Bytes below 0x80 are kept as they are (ASCII, and the control bytes the
 * clients use as colour codes); 0x80..0xFF map through the CP437 table.
 */
#ifndef PROTO10_TEXT_H
#define PROTO10_TEXT_H

#include "buf.h"

/* Converts in[0..in_len) (stops early at a NUL byte) to UTF-8 in out, NUL
   terminated. Never splits a character: the output holds at most
   out_cap - 1 bytes, so out_cap = P10_CHAT_MAX_TEXT + 1 gives a valid chat
   text. Returns the number of bytes written, without the NUL. */
size_t p10_cp437_to_utf8(const uint8_t* in, size_t in_len, char* out, size_t out_cap);

/* Converts UTF-8 to CP437 in out, NUL terminated, at most out_cap - 1
   bytes. Characters CP437 cannot show, malformed sequences and NUL become
   '?'. Returns the number of bytes written, without the NUL. */
size_t p10_utf8_to_cp437(p10_bytes in, char* out, size_t out_cap);

#endif
