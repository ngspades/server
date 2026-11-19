// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTO10_NETSTATS_H
#define PROTO10_NETSTATS_H

#include <stddef.h>
#include <stdint.h>

/* What the server sends to 1.0 clients, by packet type, before ENet's range
   coder: where the upload goes (measure first). Every
   place that sends a 1.0 packet to a peer counts it once per receiver. */
void proto10_netstats_add(const uint8_t* data, size_t size);

/* EntityUpdate items by how far the body is from its receiver */
enum {
    NETSTATS_BAND_CREATE, /* a full item: the receiver did not have the body */
    NETSTATS_BAND_OWN,    /* the receiver's own body */
    NETSTATS_BAND_32,     /* within 32 blocks (60 Hz) */
    NETSTATS_BAND_64,     /* 32..64 (30 Hz) */
    NETSTATS_BAND_128,    /* 64..128 (15 Hz) */
    NETSTATS_BAND_FAR,    /* beyond 128, the fog (5 Hz) */
    NETSTATS_BANDS
};
void proto10_netstats_band(int band, size_t bytes);

/* logs the window's figures per connected 1.0 client and starts a new
   window; total_sent is ENet's own count of the same window (compressed,
   with its headers and the acknowledgements) */
void proto10_netstats_log(double seconds, size_t clients, uint32_t total_sent);

#endif
