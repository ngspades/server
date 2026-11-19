// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Protocol 1.0 primitive codec, shared by the client and the server.
 * See docs/protocol-1.0.md section 4.
 *
 * Plain C99, no allocation, no dependency on either program. Readers and
 * writers carry a sticky error flag: once a read runs past the end (or a
 * write past the capacity) every later call is a no-op, so a packet is
 * decoded straight through and checked once at the end.
 */
#ifndef PROTO10_BUF_H
#define PROTO10_BUF_H

#include <stddef.h>
#include <stdint.h>

/* bytes and strings are decoded as views into the packet, never copied */
typedef struct {
    const uint8_t* data;
    size_t len;
} p10_bytes;

typedef struct {
    float x, y, z;
} p10_vec3;

/* r, g, b in 0..255; on the wire the order is B, G, R */
typedef struct {
    uint8_t r, g, b;
} p10_color;

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;
    int error;
} p10_reader;

typedef struct {
    uint8_t* data;
    size_t size;
    size_t cap;
    int error;
} p10_writer;

/* the reference rejects longer byte strings */
#define P10_MAX_BYTES (1024u * 1024u)

void p10_reader_init(p10_reader* r, const uint8_t* data, size_t size);
void p10_writer_init(p10_writer* w, uint8_t* data, size_t cap);

/* 1 when the whole input was consumed without error */
int p10_at_end(const p10_reader* r);
/* 1 when bytes remain and no error happened */
int p10_has_more(const p10_reader* r);

uint8_t p10_read_u8(p10_reader* r);
uint16_t p10_read_u16(p10_reader* r);
uint32_t p10_read_u32(p10_reader* r);
float p10_read_f32(p10_reader* r);
p10_vec3 p10_read_vec3(p10_reader* r);
p10_color p10_read_color(p10_reader* r);
/* unsigned LEB128, at most 10 bytes (fixes D1: the reference keeps 32 bits) */
uint64_t p10_read_varint(p10_reader* r);
/* like p10_read_varint but the value must fit in 32 bits */
uint32_t p10_read_varint32(p10_reader* r);
int64_t p10_read_svarint(p10_reader* r);
p10_bytes p10_read_bytes(p10_reader* r);
/* bytes that must not contain NUL (fixes D2: the reference truncates) */
p10_bytes p10_read_string(p10_reader* r);
/* compact quaternion: x, y, z with w >= 0 implied */
p10_vec3 p10_read_rotation(p10_reader* r);

void p10_write_raw(p10_writer* w, const void* data, size_t n);
void p10_write_u8(p10_writer* w, uint8_t v);
void p10_write_u16(p10_writer* w, uint16_t v);
void p10_write_u32(p10_writer* w, uint32_t v);
void p10_write_f32(p10_writer* w, float v);
void p10_write_vec3(p10_writer* w, p10_vec3 v);
void p10_write_color(p10_writer* w, p10_color c);
void p10_write_varint(p10_writer* w, uint64_t v);
void p10_write_svarint(p10_writer* w, int64_t v);
void p10_write_bytes(p10_writer* w, p10_bytes b);
/* writes a C string; the writer fails if it is longer than P10_MAX_BYTES */
void p10_write_cstring(p10_writer* w, const char* s);
void p10_write_rotation(p10_writer* w, p10_vec3 xyz);
/* compact form of a unit quaternion for p10_write_rotation (negated if w < 0) */
p10_vec3 p10_rotation_compact(float x, float y, float z, float w);

p10_bytes p10_bytes_from_cstring(const char* s);
/* 1 when b equals the C string s exactly */
int p10_bytes_equal(p10_bytes b, const char* s);
/* copies b into out as a NUL terminated string; 0 when it does not fit */
int p10_bytes_copy(p10_bytes b, char* out, size_t out_size);
/* 1 when b is well formed UTF-8 (names and chat must be) */
int p10_utf8_valid(p10_bytes b);

#endif
