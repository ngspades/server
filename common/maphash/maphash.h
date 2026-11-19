// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * maphash: a ngspades map's shape as one SHA-256 digest.
 *
 * Two files of one map often differ byte for byte (another editor saved it,
 * or it went through ngmap, whose decoded colours are within a step of the
 * original) and the same map has several names (a 0.75 server's "2fort" is
 * the maps repository's 2Fort_CTF). What identifies it is its shape: which
 * cells are solid. The digest is the SHA-256 of every column's solid cells
 * as an unsigned 64-bit integer (bit z set: cell z solid, z 0 the top),
 * written as 8 bytes little-endian, in the VXL file's column order.
 *
 * The same digest is tools/maplib.py's shape_hash() (the tools deduplicate
 * the maps repository by it, and tools/map_shapes.py keys shapes.json by
 * it), and the server sends it in HELLOINFO as "map_shape". Both walks must
 * stay identical: the span rules below are maplib.solid_masks'.
 *
 * Header only, like common/modes: the server and the tools' C driver both
 * include it and neither links anything.
 *
 * The SHA-256 is FIPS 180-4 written for this file.
 */
#ifndef MAPHASH_H
#define MAPHASH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* the hash needs one bit a cell and a 64-bit integer a column */
#define MAPHASH_DEPTH_MAX 64

typedef struct
{
    uint32_t h[8];
    uint64_t bits; /* the message's length, for the final block */
    uint8_t  buf[64];
    size_t   used;
} maphash_sha256_t;

static inline uint32_t maphash_rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static const uint32_t maphash_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

/* the first n bits set (n 0..64); shifting by 64 is undefined */
static inline uint64_t maphash_low_bits(int n)
{
    return n >= 64 ? ~(uint64_t) 0 : (((uint64_t) 1 << n) - 1);
}

static inline void maphash_sha256_block(maphash_sha256_t* ctx, const uint8_t* block)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t) block[4 * i] << 24 | (uint32_t) block[4 * i + 1] << 16 |
               (uint32_t) block[4 * i + 2] << 8 | (uint32_t) block[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = maphash_rotr(w[i - 15], 7) ^ maphash_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = maphash_rotr(w[i - 2], 17) ^ maphash_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i]        = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1  = maphash_rotr(e, 6) ^ maphash_rotr(e, 11) ^ maphash_rotr(e, 25);
        uint32_t ch  = (e & f) ^ (~e & g);
        uint32_t t1  = h + s1 + ch + maphash_k[i] + w[i];
        uint32_t s0  = maphash_rotr(a, 2) ^ maphash_rotr(a, 13) ^ maphash_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2  = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
    ctx->h[5] += f;
    ctx->h[6] += g;
    ctx->h[7] += h;
}

static inline void maphash_sha256_init(maphash_sha256_t* ctx)
{
    ctx->h[0] = 0x6a09e667u;
    ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u;
    ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu;
    ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu;
    ctx->h[7] = 0x5be0cd19u;
    ctx->bits = 0;
    ctx->used = 0;
}

static inline void maphash_sha256_update(maphash_sha256_t* ctx, const uint8_t* data, size_t len)
{
    ctx->bits += (uint64_t) len * 8;
    while (len > 0) {
        size_t take = sizeof ctx->buf - ctx->used;
        if (take > len) {
            take = len;
        }
        memcpy(ctx->buf + ctx->used, data, take);
        ctx->used += take;
        data += take;
        len -= take;
        if (ctx->used == sizeof ctx->buf) {
            maphash_sha256_block(ctx, ctx->buf);
            ctx->used = 0;
        }
    }
}

static inline void maphash_sha256_final(maphash_sha256_t* ctx, uint8_t out[32])
{
    static const uint8_t pad[64] = {0x80};
    uint64_t             bits    = ctx->bits;
    /* 0x80, zeros, then the length in bits big-endian: the message's own
       length, so it is taken before the padding is appended */
    size_t               n       = ctx->used < 56 ? 56 - ctx->used : 120 - ctx->used;
    uint8_t              length[8];
    maphash_sha256_update(ctx, pad, n);
    for (int i = 0; i < 8; i++) {
        length[i] = (uint8_t) (bits >> (56 - 8 * i));
    }
    maphash_sha256_update(ctx, length, sizeof length);
    for (int i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t) (ctx->h[i] >> 24);
        out[4 * i + 1] = (uint8_t) (ctx->h[i] >> 16);
        out[4 * i + 2] = (uint8_t) (ctx->h[i] >> 8);
        out[4 * i + 3] = (uint8_t) ctx->h[i];
    }
}

/* one column's solid cells (bit z for cell z) into the digest: 8 bytes
   little-endian, in the VXL's column order (y rows, x columns) */
static inline void maphash_column(maphash_sha256_t* ctx, uint64_t mask)
{
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) {
        bytes[i] = (uint8_t) (mask >> (8 * i));
    }
    maphash_sha256_update(ctx, bytes, sizeof bytes);
}

/* the digest as 64 lowercase hex digits and the NUL */
static inline void maphash_hex(maphash_sha256_t* ctx, char hex_out[65])
{
    static const char hexdigits[] = "0123456789abcdef";
    uint8_t           digest[32];
    maphash_sha256_final(ctx, digest);
    for (int i = 0; i < 32; i++) {
        hex_out[2 * i]     = hexdigits[digest[i] >> 4];
        hex_out[2 * i + 1] = hexdigits[digest[i] & 0x0f];
    }
    hex_out[64] = '\0';
}

/*
 * The shape hash of a VXL of w x h x d cells (d 1..64) into hex_out (65
 * bytes: 64 lowercase hex digits and the NUL). 0, or -1 for bytes that are
 * not a VXL of that size, a depth the hash cannot hold, or a column whose
 * span walk leaves the file (untrusted input: every read is bounds checked).
 *
 * A span (N, S, E, A) is air down to S, then solid from S to the next
 * span's A byte, or to the bottom on the last span (N 0); a column with
 * S == d and E == S - 1 has no cell at all (no water layer under it), which
 * is valid and some maps have. That is maplib.solid_masks' walk, and the
 * tools' digest changes if this one does.
 */
static inline int maphash_shape(const uint8_t* vxl, size_t len, int w, int h, int d, char hex_out[65])
{
    if (vxl == NULL || hex_out == NULL || w <= 0 || h <= 0 || d <= 0 || d > MAPHASH_DEPTH_MAX) {
        return -1;
    }
    maphash_sha256_t ctx;
    maphash_sha256_init(&ctx);
    const uint64_t full    = maphash_low_bits(d);
    size_t         p       = 0;
    size_t         columns = (size_t) w * (size_t) h;
    for (size_t c = 0; c < columns; c++) {
        uint64_t mask = 0;
        for (;;) {
            if (p > len || len - p < 4) {
                return -1;
            }
            unsigned n = vxl[p], s = vxl[p + 1], e = vxl[p + 2];
            /* s == d is an empty column, so only above d is bad */
            if (s > (unsigned) d || e >= (unsigned) d || e + 1 < s) {
                return -1;
            }
            if (n == 0) {
                mask |= full & ~maphash_low_bits((int) s);
                p += 4 * ((size_t) e + 2 - s);
                break;
            }
            p += 4 * (size_t) n;
            /* the next span's A byte ends this one's solid run */
            if (p > len || len - p < 4) {
                return -1;
            }
            unsigned a = vxl[p + 3];
            /* maplib would compute a wider mask and then fail to write it
               as 8 bytes: such a file has no shape hash either way */
            if (a > (unsigned) d) {
                return -1;
            }
            mask |= maphash_low_bits((int) a) & ~maphash_low_bits((int) s);
        }
        if (p > len) {
            return -1;
        }
        maphash_column(&ctx, mask);
    }
    /* a file that walks to its exact end is a VXL of this size */
    if (p != len) {
        return -1;
    }
    maphash_hex(&ctx, hex_out);
    return 0;
}

#endif
