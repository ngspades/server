# The map in the server's memory

How the server keeps the map, what it costs, and what the alternatives
measured: (1024 x 1024 maps, where the server's map took
294 MiB of a 1024 match) and (protocol 1.0 sends only ngmap).
Measured on an Apple M4 (2026-11-03) with `tools/mapbench/`.

## The three ways to keep a map

**libmapvxl** (SpadesX's, `server/Extern/libmapvxl`, what the server uses
today): a dense array, `blocks[x][y][z]`, 4 bytes a cell (colour and the
solid bit together), plus a pointer a column. Every cell costs the same,
air and buried ground too. Every query is a direct read, nothing to work
out. 64 MiB at 512, 256 MiB at 1024.

**libvxl** (BetterSpades', what the client uses, `vxl` fetched by the
client's CMake): the solid cells as a bitset (1 bit a cell: 2 MiB at 512,
8 MiB at 1024), and colours only for the cells that touch the air, the
only ones a VXL file has a colour for (a buried cell gets the default dirt
colour, as in the original game). The colours are (position, colour)
pairs of 8 bytes, sorted, in one list a 16 x 16 column chunk: a colour is
a binary search in a short list, an edit an insertion in it.

**libpvx2** (notaburner's, the map library of the lsd 0.75 server,
0BSD-style licence; lsd itself has none): the solid cells as one 64 bit
word a column (`MapCol`: top and floor by bit scans, ranges by masks,
collapse found column by column with bit operations, `cull.c`), and the
colours as a dense array of 3 bytes a cell, column after column
(`colorbuf[(xy * 64 + z) * 3]`). Every column has a surface, so every page
of the colour array is touched and resident: 50 MiB at 512, 200 MiB at
1024. The fastest at everything, the size fixed at compile time.

ngmap's own model (`common/ngmap/vxl.h`, `Map`) is a fourth, close to
libvxl: a 64 bit word a column, a mask of the cells with a colour, a
column offset into one global colour array (popcount gives a colour at
once). Its colours are one array for the whole map, so an edit moves
everything after it: fine for a codec, not for a live map.

## Measured

The same ngmap q100 files (the maps repository's) loaded into each
library, each case its own process (`tools/mapbench/`). 20 million random
`is_solid`, 2 million `get_color`, 200 000 edits near the surface (half
builds, half digs). The map's size is the resident memory the library
added (after the decode, before the edits).

### 512 x 512 (Raindance, Busan_Port)

| | libmapvxl (today) | libvxl | libpvx2 (lsd) |
|---|---|---|---|
| The map | 63-64 MiB | 6.5-8.8 MiB | 50 MiB |
| `is_solid` | 5.3-7.3 ns | 3.4-3.6 ns | 3.1-3.3 ns |
| `get_color` | 6.7-14.3 ns | 10-25 ns | 4.7-5.6 ns |
| an edit | 0.07-0.13 us | 0.22-0.32 us | 0.01 us |
| load (decode + build) | 40-65 ms | 29-31 ms | 22-52 ms |

### 1024 x 1024 (Caves, Skybridge, from `tools/gen_maps.py`)

| | libmapvxl (today) | libvxl | libpvx2 (lsd) |
|---|---|---|---|
| The map | 256 MiB | 29-34 MiB (8 geometry, 19-25 colours) | 200 MiB |
| `is_solid` | 12.2 ns | 3.6 ns | 3.1 ns |
| `get_color` | 13-16 ns | 24-36 ns | 6.4-10 ns |
| an edit | 0.16-0.20 us | 0.48 us | 0.01 us |
| load (decode + build) | 176-203 ms | 128-131 ms | 107-125 ms |

`is_solid` is the query the server makes millions of times a second
(physics, the bots, rays, the collapse check): a bitset fits the CPU's
caches, a dense array of 4 bytes a cell does not, so the dense one is the
slowest there and gets slower with the map (12 ns at 1024). A colour is
asked rarely (a build, the base repair, the preview, writing the map), an
edit a few thousand times a match: libvxl's half microsecond is nothing
there.

### A join

Today the server copies the whole dense map on the tick (`_raw_copy`),
writes it as VXL into a buffer of 388 bytes a column (`mapvxl_write`) and
encodes an ngmap at the client's quality (80 by default) on a thread.

| | 512 | 1024 |
|---|---|---|
| today: CPU a join | 0.2-0.7 s | 1.1-1.2 s |
| today: peak of the process | 158-160 MiB | 559-640 MiB |
| stored ngmap sent as it is | 0 ms, nothing allocated | 0 ms, nothing allocated |
| bytes sent, today's q80 re-encode | 414-715 KB | 2.76-2.85 MB |
| bytes sent, the stored q100 | 385-648 KB | 2.51-2.65 MB |

The stored q100 stream is **smaller** than the q80 one made from it (a
lossy encode of a map that was already decoded once), and it has the
colours exactly. lsd does the same in its own protocol: a map loaded and
not yet edited is sent from a pre-compressed copy (`pristineBuf`), and
compressed again only after an edit.

When the match has changed the map a lot, sending the stored stream plus
every change as TerrainUpdate grows; then the map is written and encoded
again, on a thread: 0.2-1.1 s from libvxl or libpvx2 (both write VXL a
slice at a time: `libvxl_stream`, `vxl_write(dst, map, xy, cols)`, no
388 MiB buffer).

### What the decode leaves behind

Decoding an ngmap leaves resident memory the map never uses: 17-18 MiB
after a 512 map, 73-74 MiB after a 1024 one, the same with every library
(the decoder's own model and buffers, freed, kept by the allocator;
`malloc_zone_pressure_relief` did not give it back). So a server's
resident size is the map plus that. Decoding straight into the live map
(ngmap's model is already columns of bits) or once at the download, into
a file the server then reads, would avoid it.

## What it means for the server

| A 1024 match, the server | today | libvxl + stored ngmap | libpvx2 + stored ngmap |
|---|---|---|---|
| the map | 256 MiB | ~32 MiB | 200 MiB |
| the process (measured 294 MiB today) | ~294 MiB | ~70 MiB | ~240 MiB |
| a join | 1.1 s of CPU, +250-350 MiB | nothing | nothing |
| `is_solid` | 12 ns | 3.6 ns | 3.1 ns |

The choice is libvxl: an eighth of the memory, the fast query where it
counts, one library for the client and the server (`common/`). libpvx2 is
the fastest at everything, but its dense colours keep it near today's
memory, and its size is a compile-time constant (one build a map size).
Two of its ideas are worth taking on their own: the column as one 64 bit
word (a floor, a top, a range of cells are a bit scan or a mask: the bots'
and the physics' column walks) and its collapse check by columns, against
our cell by cell `check_node` (`Nodes.c`).

## The order

Done: phases 1 and 2. The server's map is libvxl behind
`server/Source/Server/MapStore.h`. Measured with 16 bots for 600 s: the
server's peak 90 to 31 MiB on Raindance, 297 to 60 MiB on a 1024 map, the
tick the same (0.055-0.075 ms with SP_FAST); every mode event for event the
same but for the colour of a cell dug out after being buried (the dirt
colour, as on every client).

Done too: phase 3. Protocol 1.0 sends the stored ngmap: Raindance's
encoding at load took 159 ms on a thread and was 647 876 bytes, the maps
repository's stream to the byte; the next load found it kept and encoded
nothing; a second join hit the client's map cache and received 0 bytes; a
late joiner on 2Fort got the 32 KB stream and 3220 changed cells.

1. The server's map in libvxl, behind the `mapvxl_*` calls it makes today
   (`is_solid`, `get_color`, `set_color`, `set_air`, `find_top_block`,
   `write`), so the ~200 call sites do not change. Checked with
   `tools/bench.py check` (every mode event for event the same: the
   colours are the same, the solid cells are the same), the `Tick:` line,
   and the resident size at 512 and 1024.
2. Protocol 1.0 sends only ngmap: the stored q100 stream as it is, the
   changes since the load as TerrainUpdate, a new encode only past a
   number of changes. The VXL transfer (`map_quality 0`) goes; the 0.75
   profile writes VXL from libvxl a slice at a time.
3. The decode's leftover memory: decode once at the download into what the
   server loads, or decode straight into libvxl.
4. Optional: a column's floor and top by bit scans (libpvx2's idea) where
   the bots and the physics walk columns, and the collapse check by
   columns.

## After the move (phases 1 to 5)

Measured on the same Apple M4 (2026-11-04), the server alone with 16 bots,
60 s (`/usr/bin/time -l`), the map loaded from the `.ngmap` beside its VXL;
the client joining with `scripts/map_1024.txt`:

| | before phase 4 | ngmap decoded straight into libvxl |
|---|---|---|
| server peak, Raindance | 46.9 MiB | 39.7 MiB |
| server peak, Caves_1024_1 | 98.5 MiB | 90.2 MiB |
| client peak, Caves_1024_1 | 404 MiB | 318 MiB |
| client disk a 1.0 map | the stream + its VXL (18.7 MB at 1024) | the stream |

What is left of the server's load peak is the decoder's own model (a mask
and an offset a column, the colours), which it needs until the stream is
read: the VXL bytes and their parse were the part we could drop.

A 0.75 join, Raindance: the whole map written into a buffer reserved for
388 bytes a column (101 MiB at 512) and deflated on the tick, a 166 ms
tick, for every joiner; now a slice of columns at a time into deflate, 50
ms for the first joiner, nothing for the next ones while the map is the
same. The bytes are libvxl_write's, the same a 0.75 client got.

Left (phase 6, optional): a column's floor and top by bit scans where the
bots and the physics walk columns, and the collapse check by columns.
