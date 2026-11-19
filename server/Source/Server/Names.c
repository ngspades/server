// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Server/Names.h>
#include <Server/EventLog.h>
#include <Server/Structs/PlayerStruct.h>
#include <Server/Structs/ServerStruct.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NAME_WORK 256

/* one UTF-8 code point; invalid bytes are consumed one at a time and return
   0 so the caller can drop them instead of copying a broken sequence */
static size_t _utf8_next(const unsigned char* s, size_t n, size_t* at, uint32_t* cp)
{
    if (*at >= n) {
        return 0;
    }
    unsigned char c = s[*at];
    if (c < 0x80) {
        *cp = c;
        (*at)++;
        return 1;
    }
    size_t len;
    uint32_t v;
    if ((c & 0xE0) == 0xC0) {
        len = 2;
        v   = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        v   = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        v   = c & 0x07;
    } else {
        (*at)++;
        return 0;
    }
    if (*at + len > n) {
        (*at)++;
        return 0;
    }
    for (size_t k = 1; k < len; k++) {
        if ((s[*at + k] & 0xC0) != 0x80) {
            (*at)++;
            return 0;
        }
        v = (v << 6) | (s[*at + k] & 0x3F);
    }
    /* overlong, surrogate and out of range sequences are not a name */
    if ((len == 2 && v < 0x80) || (len == 3 && v < 0x800) || (len == 4 && v < 0x10000) || (v >= 0xD800 && v <= 0xDFFF) ||
        v > 0x10FFFF)
    {
        (*at)++;
        return 0;
    }
    *cp = v;
    *at += len;
    return len;
}

/* zero-width, bidi controls and other characters that draw nothing: they
   let one displayed name look like another */
static int _invisible(uint32_t cp)
{
    return cp == 0x00AD || cp == 0x034F || cp == 0x061C || cp == 0x115F || cp == 0x1160 || cp == 0x17B4 ||
           cp == 0x17B5 || (cp >= 0x180B && cp <= 0x180E) || (cp >= 0x200B && cp <= 0x200F) ||
           (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
           cp == 0xFEFF || (cp >= 0xFFF9 && cp <= 0xFFFB) || (cp >= 0xE0100 && cp <= 0xE01EF);
}

/* the Latin letter a code point is usually drawn as, or 0 */
static char _confusable(uint32_t cp)
{
    if (cp >= 0xFF01 && cp <= 0xFF5E) {
        cp -= 0xFEE0; /* full width ASCII */
    }
    switch (cp) {
        /* Cyrillic capitals and small letters */
        case 0x0410: case 0x0430: return 'a';
        case 0x0412: case 0x0432: return 'b';
        case 0x0415: case 0x0435: return 'e';
        case 0x041A: case 0x043A: return 'k';
        case 0x041C: case 0x043C: return 'm';
        case 0x041D: case 0x043D: return 'h';
        case 0x041E: case 0x043E: return 'o';
        case 0x0420: case 0x0440: return 'p';
        case 0x0421: case 0x0441: return 'c';
        case 0x0422: case 0x0442: return 't';
        case 0x0423: case 0x0443: return 'y';
        case 0x0425: case 0x0445: return 'x';
        case 0x0406: case 0x0456: return 'i';
        case 0x0408: case 0x0458: return 'j';
        case 0x0405: case 0x0455: return 's';
        /* Greek capitals and small letters */
        case 0x0391: case 0x03B1: return 'a';
        case 0x0392: case 0x03B2: return 'b';
        case 0x0395: case 0x03B5: return 'e';
        case 0x0396: case 0x03B6: return 'z';
        case 0x0397: case 0x03B7: return 'h';
        case 0x0399: case 0x03B9: return 'i';
        case 0x039A: case 0x03BA: return 'k';
        case 0x039C: case 0x03BC: return 'm';
        case 0x039D: case 0x03BD: return 'n';
        case 0x039F: case 0x03BF: return 'o';
        case 0x03A1: case 0x03C1: return 'p';
        case 0x03A4: case 0x03C4: return 't';
        case 0x03A5: case 0x03C5: return 'y';
        case 0x03A7: case 0x03C7: return 'x';
        default: return 0;
    }
}

/* a name fit to show: valid UTF-8, no invisible or control characters, no
   leading '#', no runs of spaces, at most NAME_WORK-1 bytes */
static void _clean(const char* raw, char* out, size_t size)
{
    const unsigned char* s = (const unsigned char*) (raw != NULL ? raw : "");
    size_t               n = strlen((const char*) s), at = 0;
    char                 work[NAME_WORK];
    size_t               w = 0;
    int                  space = 0;
    while (at < n) {
        uint32_t cp = 0;
        size_t   before = at;
        if (_utf8_next(s, n, &at, &cp) == 0) {
            continue; /* a broken byte is not part of a name */
        }
        if (_invisible(cp) || cp < 0x20 || cp == 0x7F) {
            continue;
        }
        if (cp == ' ' || cp == '\t') {
            space = 1;
            continue;
        }
        if (space && w > 0 && w + 1 < sizeof work) {
            work[w++] = ' ';
        }
        space = 0;
        if (cp == '#' && w == 0) {
            continue;
        }
        if (cp < 0x80) {
            if (w + 1 < sizeof work) {
                work[w++] = (char) cp;
            }
        } else {
            size_t len = at - before;
            if (w + len < sizeof work) {
                memcpy(work + w, s + before, len);
                w += len;
            }
        }
    }
    while (w > 0 && work[w - 1] == ' ') {
        w--;
    }
    if (w == 0) {
        snprintf(work, sizeof work, "Deuce");
        w = strlen(work);
    }
    if (size == 0) {
        return;
    }
    if (w >= size) {
        /* cut on a UTF-8 boundary, never in the middle of a character */
        w = size - 1;
        while (w > 0 && ((unsigned char) work[w] & 0xC0) == 0x80) {
            w--;
        }
    }
    memcpy(out, work, w);
    out[w] = '\0';
}

void names_skeleton(const char* name, char* out, size_t size)
{
    const unsigned char* s = (const unsigned char*) (name != NULL ? name : "");
    size_t               n = strlen((const char*) s), at = 0, w = 0;
    if (size == 0) {
        return;
    }
    while (at < n) {
        uint32_t cp = 0;
        size_t   before = at;
        if (_utf8_next(s, n, &at, &cp) == 0) {
            continue;
        }
        char c = _confusable(cp);
        if (c == 0 && cp < 0x80) {
            c = (char) tolower((unsigned char) cp);
        }
        if (c != 0) {
            if (c == '0') c = 'o';
            if (c == '1' || c == '|' || c == '!') c = 'i';
            if (c == '3') c = 'e';
            if (c == '4' || c == '@') c = 'a';
            if (c == '5' || c == '$') c = 's';
            if (c == '7') c = 't';
            if (c == '8') c = 'b';
            if (c == '9') c = 'g';
            if (isalnum((unsigned char) c) && w + 1 < size) {
                out[w++] = c;
            }
            continue;
        }
        if (cp >= 0x80 && !_invisible(cp)) {
            /* no Latin lookalike: keep the bytes, so two different names in
               another script do not become the same skeleton */
            size_t len = at - before;
            if (w + len < size) {
                memcpy(out + w, s + before, len);
                w += len;
            }
        }
    }
    out[w] = '\0';
}

static int _unwanted(const char* skeleton)
{
    /* the old filter's list, now on the skeleton so 1gg3r and igger are the
       same word; the display name is replaced, not merely hidden */
    return strstr(skeleton, "igger") != NULL || strstr(skeleton, "1gger") != NULL ||
           strstr(skeleton, "igg3r") != NULL || strstr(skeleton, "1gg3r") != NULL;
}

static int _reserved(const char* skeleton)
{
    static const char* reserved[] = {"server", "admin", "administrator", "console", "system", "moderator", "owner",
                                     "root", "staff", "host", NULL};
    for (int k = 0; reserved[k] != NULL; k++) {
        if (!strcmp(skeleton, reserved[k])) {
            return 1;
        }
    }
    return 0;
}

static int _taken(server_t* server, player_t* self, const char* skeleton)
{
    player_t *p, *tmp;
    char      other[NAME_WORK];
    HASH_ITER(hh, server->players, p, tmp)
    {
        if (p == self || p->state == STATE_DISCONNECTED) {
            continue;
        }
        names_skeleton(p->name, other, sizeof other);
        if (!strcmp(skeleton, other)) {
            return 1;
        }
    }
    return 0;
}

uint8_t names_apply(server_t* server, player_t* player, const char* raw, char* out, size_t size)
{
    char cleaned[NAME_WORK];
    char base[NAME_WORK];
    char skeleton[NAME_WORK];
    char candidate[NAME_WORK];
    _clean(raw, cleaned, sizeof cleaned);
    snprintf(base, sizeof base, "%s", cleaned);
    names_skeleton(base, skeleton, sizeof skeleton);
    const char* why = "clean";
    if (_unwanted(skeleton)) {
        snprintf(base, sizeof base, "Player");
        names_skeleton(base, skeleton, sizeof skeleton);
        why = "unwanted";
    } else if (_reserved(skeleton)) {
        snprintf(base, sizeof base, "Player");
        names_skeleton(base, skeleton, sizeof skeleton);
        why = "reserved";
    }
    snprintf(candidate, sizeof candidate, "%s", base);
    /* the id suffix is stable and readable; the loop is for the rare case
       where another player already uses it too */
    for (int attempt = 0; attempt < 100 && _taken(server, player, skeleton); attempt++) {
        if (attempt == 0) {
            snprintf(candidate, sizeof candidate, "%s-%u", base, (unsigned) player->id);
        } else {
            snprintf(candidate, sizeof candidate, "%s-%u-%d", base, (unsigned) player->id, attempt + 1);
        }
        names_skeleton(candidate, skeleton, sizeof skeleton);
        why = "unique";
    }
    _clean(candidate, candidate, sizeof candidate);
    if (size > 0) {
        snprintf(out, size, "%s", candidate);
    }
    uint8_t changed = raw == NULL || strcmp(raw, out) != 0;
    if (changed) {
        evt_log(player, "name", "action=%s id=%u name=\"%s\"", why, (unsigned) player->id, out);
    }
    return changed;
}
