// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "buf.h"

#include <string.h>

void p10_reader_init(p10_reader* r, const uint8_t* data, size_t size)
{
    r->data  = data;
    r->size  = data ? size : 0;
    r->pos   = 0;
    r->error = 0;
}

void p10_writer_init(p10_writer* w, uint8_t* data, size_t cap)
{
    w->data  = data;
    w->size  = 0;
    w->cap   = data ? cap : 0;
    w->error = 0;
}

int p10_at_end(const p10_reader* r)
{
    return !r->error && r->pos == r->size;
}

int p10_has_more(const p10_reader* r)
{
    return !r->error && r->pos < r->size;
}

static int need(p10_reader* r, size_t n)
{
    if (r->error || r->size - r->pos < n) {
        r->error = 1;
        return 0;
    }
    return 1;
}

uint8_t p10_read_u8(p10_reader* r)
{
    return need(r, 1) ? r->data[r->pos++] : 0;
}

uint16_t p10_read_u16(p10_reader* r)
{
    if (!need(r, 2))
        return 0;
    const uint8_t* p = r->data + r->pos;
    r->pos += 2;
    return (uint16_t) (p[0] | (p[1] << 8));
}

uint32_t p10_read_u32(p10_reader* r)
{
    if (!need(r, 4))
        return 0;
    const uint8_t* p = r->data + r->pos;
    r->pos += 4;
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

float p10_read_f32(p10_reader* r)
{
    uint32_t u = p10_read_u32(r);
    float f;
    /* memcpy instead of a union: no aliasing questions on any compiler */
    memcpy(&f, &u, sizeof f);
    return f;
}

p10_vec3 p10_read_vec3(p10_reader* r)
{
    p10_vec3 v;
    v.x = p10_read_f32(r);
    v.y = p10_read_f32(r);
    v.z = p10_read_f32(r);
    return v;
}

p10_color p10_read_color(p10_reader* r)
{
    p10_color c;
    c.b = p10_read_u8(r);
    c.g = p10_read_u8(r);
    c.r = p10_read_u8(r);
    return c;
}

uint64_t p10_read_varint(p10_reader* r)
{
    uint64_t v = 0;
    for (int shift = 0; shift < 70; shift += 7) {
        uint8_t b = p10_read_u8(r);
        if (r->error)
            return 0;
        /* the 10th byte may only carry the top bit of a 64-bit value */
        if (shift == 63 && (b & 0x7e)) {
            r->error = 1;
            return 0;
        }
        v |= (uint64_t) (b & 0x7f) << shift;
        if (!(b & 0x80))
            return v;
    }
    r->error = 1;
    return 0;
}

uint32_t p10_read_varint32(p10_reader* r)
{
    uint64_t v = p10_read_varint(r);
    if (v > UINT32_MAX) {
        r->error = 1;
        return 0;
    }
    return (uint32_t) v;
}

int64_t p10_read_svarint(p10_reader* r)
{
    uint64_t u = p10_read_varint(r);
    return (u & 1) ? -1 - (int64_t) (u >> 1) : (int64_t) (u >> 1);
}

p10_bytes p10_read_bytes(p10_reader* r)
{
    p10_bytes b = {NULL, 0};
    uint64_t n  = p10_read_varint(r);
    if (r->error)
        return b;
    if (n > P10_MAX_BYTES || !need(r, (size_t) n)) {
        r->error = 1;
        return b;
    }
    b.data = r->data + r->pos;
    b.len  = (size_t) n;
    r->pos += (size_t) n;
    return b;
}

p10_bytes p10_read_string(p10_reader* r)
{
    p10_bytes b = p10_read_bytes(r);
    if (b.len && memchr(b.data, 0, b.len)) {
        r->error = 1;
        b.data   = NULL;
        b.len    = 0;
    }
    return b;
}

p10_vec3 p10_read_rotation(p10_reader* r)
{
    return p10_read_vec3(r);
}

void p10_write_raw(p10_writer* w, const void* data, size_t n)
{
    if (w->error || w->cap - w->size < n) {
        w->error = 1;
        return;
    }
    if (n)
        memcpy(w->data + w->size, data, n);
    w->size += n;
}

void p10_write_u8(p10_writer* w, uint8_t v)
{
    p10_write_raw(w, &v, 1);
}

void p10_write_u16(p10_writer* w, uint16_t v)
{
    uint8_t b[2] = {(uint8_t) v, (uint8_t) (v >> 8)};
    p10_write_raw(w, b, 2);
}

void p10_write_u32(p10_writer* w, uint32_t v)
{
    uint8_t b[4] = {(uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24)};
    p10_write_raw(w, b, 4);
}

void p10_write_f32(p10_writer* w, float v)
{
    uint32_t u;
    memcpy(&u, &v, sizeof u);
    p10_write_u32(w, u);
}

void p10_write_vec3(p10_writer* w, p10_vec3 v)
{
    p10_write_f32(w, v.x);
    p10_write_f32(w, v.y);
    p10_write_f32(w, v.z);
}

void p10_write_color(p10_writer* w, p10_color c)
{
    p10_write_u8(w, c.b);
    p10_write_u8(w, c.g);
    p10_write_u8(w, c.r);
}

void p10_write_varint(p10_writer* w, uint64_t v)
{
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        p10_write_u8(w, v ? (uint8_t) (b | 0x80) : b);
    } while (v);
}

void p10_write_svarint(p10_writer* w, int64_t v)
{
    p10_write_varint(w, v < 0 ? (((uint64_t) (-1 - v)) << 1) | 1 : ((uint64_t) v) << 1);
}

void p10_write_bytes(p10_writer* w, p10_bytes b)
{
    if (b.len > P10_MAX_BYTES) {
        w->error = 1;
        return;
    }
    p10_write_varint(w, b.len);
    p10_write_raw(w, b.data, b.len);
}

void p10_write_cstring(p10_writer* w, const char* s)
{
    p10_write_bytes(w, p10_bytes_from_cstring(s));
}

void p10_write_rotation(p10_writer* w, p10_vec3 xyz)
{
    p10_write_vec3(w, xyz);
}

p10_vec3 p10_rotation_compact(float x, float y, float z, float w)
{
    p10_vec3 v;
    float s = w < 0.0f ? -1.0f : 1.0f;
    v.x     = x * s;
    v.y     = y * s;
    v.z     = z * s;
    return v;
}

p10_bytes p10_bytes_from_cstring(const char* s)
{
    p10_bytes b;
    b.data = (const uint8_t*) s;
    b.len  = s ? strlen(s) : 0;
    return b;
}

int p10_bytes_equal(p10_bytes b, const char* s)
{
    size_t n = strlen(s);
    return b.len == n && (n == 0 || memcmp(b.data, s, n) == 0);
}

int p10_bytes_copy(p10_bytes b, char* out, size_t out_size)
{
    if (out_size == 0)
        return 0;
    if (b.len >= out_size || (b.len && memchr(b.data, 0, b.len))) {
        out[0] = '\0';
        return 0;
    }
    if (b.len)
        memcpy(out, b.data, b.len);
    out[b.len] = '\0';
    return 1;
}

int p10_utf8_valid(p10_bytes b)
{
    size_t i = 0;
    while (i < b.len) {
        uint8_t c = b.data[i];
        size_t n;
        uint32_t cp;
        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xe0) == 0xc0) {
            n  = 1;
            cp = c & 0x1f;
        } else if ((c & 0xf0) == 0xe0) {
            n  = 2;
            cp = c & 0x0f;
        } else if ((c & 0xf8) == 0xf0) {
            n  = 3;
            cp = c & 0x07;
        } else {
            return 0;
        }
        if (b.len - i - 1 < n)
            return 0;
        for (size_t k = 1; k <= n; k++) {
            uint8_t cc = b.data[i + k];
            if ((cc & 0xc0) != 0x80)
                return 0;
            cp = (cp << 6) | (cc & 0x3f);
        }
        /* overlong forms, surrogates and values past U+10FFFF */
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff)
            return 0;
        i += n + 1;
    }
    return 1;
}
