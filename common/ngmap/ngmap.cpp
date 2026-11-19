/*
 * ngmap codec, ported from OpenSpades' ngspades branch
 * (Sources/Client/GameMap_ngmap.cpp, Copyright (c) 2013 yvt, GPLv3, based on
 * pysnip by Mathias Kaerlev). The bitstream is kept identical to the
 * reference so maps can be exchanged with it; only the plumbing changed:
 * GameMap/IStream/Bitmap are replaced by a sparse voxel map, a zlib reader
 * and jpge/stb_image, and every read is bounds checked because the decoder
 * consumes data from untrusted servers.
 *
 * Stream (zlib-wrapped deflate, i.e. deflateInit() not raw deflate):
 *   u32 signature, u16 w, u16 h, u16 d, u8 quality,
 *   geometry (one entry per column, boustrophedon rows),
 *   u32 jpegLength, jpeg, colour blocks (8x8x8 cells) to the end.
 */
#include "ngmap.h"
#include "vxl.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

#include <zlib.h>

#include "jpge.h"

// stb_image.h v2.30, vendored unmodified from
// https://github.com/nothings/stb/blob/master/stb_image.h (public domain or
// MIT, at the user's choice). Only its JPEG decoder is compiled in.
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
// the tile atlas of a 2048x2048 map is about 6000 pixels wide
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace ngmap {

static const uint32_t kSignature = 0x1145148a;

enum class ColumnFormat { Rle = 0, Bitmap = 1 };

enum class BlockFormat {
    Constant = 0x00,   // one colour for every voxel
    LinearX = 0x01,    // 1D image along X
    LinearY = 0x02,
    LinearZ = 0x03,
    PlanarX = 0x04,    // 2D image repeated along X
    PlanarY = 0x05,
    PlanarZ = 0x06,
    Volumetric = 0x07  // 3D image
};

enum : uint8_t {
    kLinearLinear = 0x00, // gradient between 2 colours
    kLinearRaw = 0x20,
    kPlanarLinear = 0x00, // 4-corner interpolation
    kPlanarLossy = 0x10,  // tile in the JPEG
    kPlanarRaw = 0x20,
    kVolumetricRaw = 0x10
};

static uint8_t MakeFormat(BlockFormat f, uint8_t sub) { return static_cast<uint8_t>(static_cast<uint8_t>(f) | sub); }

// The reference's IntVector3 with just the operations the codec uses.
// Division truncates toward zero, as in the reference.
struct IV {
    int x = 0, y = 0, z = 0;
    IV() = default;
    IV(int a, int b, int c) : x(a), y(b), z(c) {}
    IV& operator+=(const IV& o) { x += o.x; y += o.y; z += o.z; return *this; }
    IV operator+(const IV& o) const { return IV(x + o.x, y + o.y, z + o.z); }
    IV operator-(const IV& o) const { return IV(x - o.x, y - o.y, z - o.z); }
    IV operator*(int s) const { return IV(x * s, y * s, z * s); }
    IV& operator*=(int s) { x *= s; y *= s; z *= s; return *this; }
    IV& operator/=(int s) { x /= s; y /= s; z /= s; return *this; }
    bool operator==(const IV& o) const { return x == o.x && y == o.y && z == o.z; }
    static int Dot(const IV& a, const IV& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
};

struct LinearColorBlock {
    IV colors[8];
};
struct PlanarColorBlock {
    IV colors[8][8];
};

static uint32_t IVToColor(const IV& v) {
    return static_cast<uint32_t>(v.x & 0xff) | static_cast<uint32_t>(v.y & 0xff) << 8 |
           static_cast<uint32_t>(v.z & 0xff) << 16;
}
static IV IVFromColor(uint32_t c) {
    return IV(static_cast<int>(c & 0xff), static_cast<int>((c >> 8) & 0xff), static_cast<int>((c >> 16) & 0xff));
}

// Tracks the per-channel bounding box of a set of colours; the reference
// repeats this inline in every ComputeDiversity* function.
struct ColorRange {
    IV mx, mn;
    bool has = false;
    void Add(const IV& c) {
        if (!has) {
            mx = mn = c;
            has = true;
            return;
        }
        mx.x = std::max(mx.x, c.x); mx.y = std::max(mx.y, c.y); mx.z = std::max(mx.z, c.z);
        mn.x = std::min(mn.x, c.x); mn.y = std::min(mn.y, c.y); mn.z = std::min(mn.z, c.z);
    }
    int Diversity() const {
        if (!has)
            return 0;
        IV d = mx - mn;
        return IV::Dot(d, d);
    }
};

// One 8x8x8 cell. needscolor[x][y] bit z: voxel needs a colour.
struct ColorBlock {
    uint8_t needscolor[8][8];
    uint32_t colors[8][8][8];

    ColorBlock() {
        std::memset(needscolor, 0, sizeof(needscolor));
        std::memset(colors, 0, sizeof(colors));
    }

    IV At(int x, int y, int z) const { return IVFromColor(colors[x][y][z]); }

    bool GetNeedsColor() const {
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++)
                if (needscolor[x][y])
                    return true;
        return false;
    }

    uint8_t GetNeedsColorMapLinearX() const {
        uint8_t ret = 0;
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++)
                if (needscolor[x][y])
                    ret = static_cast<uint8_t>(ret | (1 << x));
        return ret;
    }
    uint8_t GetNeedsColorMapLinearY() const {
        uint8_t ret = 0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                if (needscolor[x][y])
                    ret = static_cast<uint8_t>(ret | (1 << y));
        return ret;
    }
    uint8_t GetNeedsColorMapLinearZ() const {
        uint8_t ret = 0;
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++)
                ret = static_cast<uint8_t>(ret | needscolor[x][y]);
        return ret;
    }
    // planar maps: ret[u] bit v, where (u, v) are the image axes
    void GetNeedsColorMapPlanarX(uint8_t ret[8]) const { // (y, z)
        std::fill(ret, ret + 8, 0);
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++)
                ret[y] = static_cast<uint8_t>(ret[y] | needscolor[x][y]);
    }
    void GetNeedsColorMapPlanarY(uint8_t ret[8]) const { // (x, z)
        std::fill(ret, ret + 8, 0);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                ret[x] = static_cast<uint8_t>(ret[x] | needscolor[x][y]);
    }
    void GetNeedsColorMapPlanarZ(uint8_t ret[8]) const { // (x, y)
        for (int x = 0; x < 8; x++) {
            uint8_t r = 0;
            for (int y = 0; y < 8; y++)
                if (needscolor[x][y])
                    r = static_cast<uint8_t>(r | (1 << y));
            ret[x] = r;
        }
    }

    // Calls f(x, y, z) for every voxel that needs a colour.
    template <class F> void ForEach(F f) const {
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++) {
                int b = needscolor[x][y];
                if (!b)
                    continue;
                for (int z = 0; z < 8; z++)
                    if (b & (1 << z))
                        f(x, y, z);
            }
    }

    IV ToConstant() const {
        IV sum;
        int count = 0;
        ForEach([&](int x, int y, int z) { sum += At(x, y, z); count++; });
        if (count > 1)
            sum /= count;
        return sum;
    }
    void FromConstant(const IV& c) {
        uint32_t cc = IVToColor(c);
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++)
                for (int z = 0; z < 8; z++)
                    colors[x][y][z] = cc;
    }

    // axis: 0 = X, 1 = Y, 2 = Z
    void ToLinear(int axis, LinearColorBlock& o) const {
        IV sum[8];
        int count[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        ForEach([&](int x, int y, int z) {
            int i = axis == 0 ? x : axis == 1 ? y : z;
            sum[i] += At(x, y, z);
            count[i]++;
        });
        for (int i = 0; i < 8; i++) {
            if (count[i] > 1)
                sum[i] /= count[i];
            o.colors[i] = sum[i];
        }
    }
    void FromLinear(int axis, const LinearColorBlock& o) {
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++)
                for (int z = 0; z < 8; z++)
                    colors[x][y][z] = IVToColor(o.colors[axis == 0 ? x : axis == 1 ? y : z]);
    }

    // Image coordinates (u, v) of voxel (x, y, z) for a planar format whose
    // image is repeated along `axis`: X -> (y, z), Y -> (x, z), Z -> (x, y).
    static void PlanarUV(int axis, int x, int y, int z, int& u, int& v) {
        if (axis == 0) { u = y; v = z; }
        else if (axis == 1) { u = x; v = z; }
        else { u = x; v = y; }
    }
    void ToPlanar(int axis, PlanarColorBlock& o) const {
        int count[8][8];
        std::memset(count, 0, sizeof(count));
        for (auto& row : o.colors)
            for (auto& c : row)
                c = IV();
        ForEach([&](int x, int y, int z) {
            int u, v;
            PlanarUV(axis, x, y, z, u, v);
            o.colors[u][v] += At(x, y, z);
            count[u][v]++;
        });
        for (int u = 0; u < 8; u++)
            for (int v = 0; v < 8; v++)
                if (count[u][v] > 1)
                    o.colors[u][v] /= count[u][v];
    }
    void FromPlanar(int axis, const PlanarColorBlock& o) {
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++)
                for (int z = 0; z < 8; z++) {
                    int u, v;
                    PlanarUV(axis, x, y, z, u, v);
                    colors[x][y][z] = IVToColor(o.colors[u][v]);
                }
    }

    int ComputeDiversity() const {
        ColorRange r;
        ForEach([&](int x, int y, int z) { r.Add(At(x, y, z)); });
        return r.Diversity();
    }
    // Worst diversity among the voxels sharing one linear coordinate.
    int ComputeDiversity1D(int axis) const {
        ColorRange r[8];
        ForEach([&](int x, int y, int z) { r[axis == 0 ? x : axis == 1 ? y : z].Add(At(x, y, z)); });
        int maxdiv = 0;
        for (auto& i : r)
            maxdiv = std::max(maxdiv, i.Diversity());
        return maxdiv;
    }
    // Worst diversity among the voxels sharing one planar coordinate.
    int ComputeDiversity2D(int axis) const {
        ColorRange r[8][8];
        ForEach([&](int x, int y, int z) {
            int u, v;
            PlanarUV(axis, x, y, z, u, v);
            r[u][v].Add(At(x, y, z));
        });
        int maxdiv = 0;
        for (auto& row : r)
            for (auto& i : row)
                maxdiv = std::max(maxdiv, i.Diversity());
        return maxdiv;
    }
};

static int AxisOf(BlockFormat f) {
    switch (f) {
    case BlockFormat::LinearX: case BlockFormat::PlanarX: return 0;
    case BlockFormat::LinearY: case BlockFormat::PlanarY: return 1;
    default: return 2;
    }
}

static uint8_t LinearNeeds(const ColorBlock& b, int axis) {
    return axis == 0 ? b.GetNeedsColorMapLinearX() : axis == 1 ? b.GetNeedsColorMapLinearY() : b.GetNeedsColorMapLinearZ();
}
static void PlanarNeeds(const ColorBlock& b, int axis, uint8_t out[8]) {
    if (axis == 0)
        b.GetNeedsColorMapPlanarX(out);
    else if (axis == 1)
        b.GetNeedsColorMapPlanarY(out);
    else
        b.GetNeedsColorMapPlanarZ(out);
}

/*
 * Which voxels of the cell at (x, y, z) are visible and need a colour.
 * Ported verbatim, quirks included, because encoder and decoder must agree
 * bit for bit: out-of-map columns count as air, the layer above z = 0 counts
 * as air, and the layer d - 1 and below counts as solid ("earth"), which also
 * flags the bottom layer of out-of-map columns and, for depths under ~24,
 * leaks earth bits into real voxels. Callers ignore flags on voxels that are
 * not solid or not in the map, but must still consume their colour data.
 */
static void ComputeNeedsColor(const Map& m, int x, int y, int z, uint8_t needscolor[8][8]) {
    uint16_t subsolidmap[10][10];
    uint16_t earth = 0;
    if (z + 8 >= m.d) {
        int b = m.d - z;
        if (b <= 0)
            earth = 0xffff;
        else if (b < 16)
            earth = static_cast<uint16_t>(~((1 << b) - 1));
    }
    for (int cx = 0; cx < 10; cx++) {
        for (int cy = 0; cy < 10; cy++) {
            int xx = cx + x - 1, yy = cy + y - 1;
            uint64_t mm = (xx >= 0 && yy >= 0 && xx < m.w && yy < m.h) ? m.Solid(xx, yy) : 0;
            mm |= earth;
            if (z == 0)
                mm <<= 1;
            else
                mm >>= z - 1;
            subsolidmap[cx][cy] = static_cast<uint16_t>(static_cast<uint16_t>(mm & 0x3ff) | earth);
        }
    }
    for (int cx = 0; cx < 8; cx++) {
        for (int cy = 0; cy < 8; cy++) {
            int b = subsolidmap[cx + 1][cy + 1];
            int result = b & ~static_cast<int>(subsolidmap[cx][cy + 1]);
            result |= b & ~static_cast<int>(subsolidmap[cx + 2][cy + 1]);
            result |= b & ~static_cast<int>(subsolidmap[cx + 1][cy]);
            result |= b & ~static_cast<int>(subsolidmap[cx + 1][cy + 2]);
            result |= b & ~(b >> 1);
            result |= b & ~(b << 1);
            needscolor[cx][cy] = static_cast<uint8_t>(result >> 1);
        }
    }
}

// Odd columns of cells walk z from the bottom up, as in the reference.
static int CellZ(int x, int zz, int d) { return (x & 8) ? (((d + 7) & ~7) - 8 - zz) : zz; }

// ---- decoder ----

[[noreturn]] static void Corrupt(const char* why) { throw Error(NGMAP_ERR_CORRUPT, why); }

// Pulls bytes out of the zlib stream on demand, so the decoder never holds
// the whole decompressed stream (a few hundred MB for a hostile map).
class Inflater {
    z_stream zs;
    bool open = false;
    bool ended = false;
    uint8_t buf[65536];
    size_t pos = 0, len = 0;

    void Fill() {
        if (ended)
            Corrupt("stream truncated");
        zs.next_out = buf;
        zs.avail_out = sizeof(buf);
        int ret = inflate(&zs, Z_NO_FLUSH);
        if (ret == Z_STREAM_END)
            ended = true;
        else if (ret != Z_OK)
            Corrupt(ret == Z_MEM_ERROR ? "out of memory" : "deflate data corrupted");
        len = sizeof(buf) - zs.avail_out;
        pos = 0;
        if (len == 0 && (ended || zs.avail_in == 0))
            Corrupt("stream truncated");
    }

public:
    Inflater(const uint8_t* data, size_t size) {
        std::memset(&zs, 0, sizeof(zs));
        if (size > UINT_MAX)
            throw Error(NGMAP_ERR_TOO_LARGE, "stream too large");
        zs.next_in = const_cast<Bytef*>(data);
        zs.avail_in = static_cast<uInt>(size);
        if (inflateInit(&zs) != Z_OK)
            throw Error(NGMAP_ERR_NOMEM, "inflateInit failed");
        open = true;
    }
    ~Inflater() {
        if (open)
            inflateEnd(&zs);
    }
    Inflater(const Inflater&) = delete;
    Inflater& operator=(const Inflater&) = delete;

    uint8_t Byte() {
        while (pos == len)
            Fill();
        return buf[pos++];
    }
    uint16_t U16() {
        uint16_t lo = Byte();
        return static_cast<uint16_t>(lo | Byte() << 8);
    }
    uint32_t U32() {
        uint32_t v = U16();
        return v | static_cast<uint32_t>(U16()) << 16;
    }
    void Read(uint8_t* out, size_t n) {
        while (n > 0) {
            while (pos == len)
                Fill();
            size_t k = std::min(n, len - pos);
            std::memcpy(out, buf + pos, k);
            out += k;
            pos += k;
            n -= k;
        }
    }
    IV Color() {
        int r = Byte(), g = Byte();
        return IV(r, g, Byte());
    }
};

struct StbiImage {
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ~StbiImage() { stbi_image_free(px); }
};

class Decoder {
    Inflater& in;
    Map& map;
    StbiImage lossy;
    uint32_t lossyIndex = 0;

    static void DecodeLinearLinear(const IV& c1, const IV& c2, LinearColorBlock& sub) {
        int v1 = c1.x * 7, d1 = c2.x - c1.x;
        int v2 = c1.y * 7, d2 = c2.y - c1.y;
        int v3 = c1.z * 7, d3 = c2.z - c1.z;
        for (int i = 0; i < 8; i++) {
            sub.colors[i] = IV(v1 / 7, v2 / 7, v3 / 7);
            v1 += d1; v2 += d2; v3 += d3;
        }
    }

    friend class Emitter;

    void DecodeLinear(uint8_t fmtcode, ColorBlock& block) {
        int axis = AxisOf(static_cast<BlockFormat>(fmtcode & 0xf));
        uint8_t needs = LinearNeeds(block, axis);
        LinearColorBlock sub;
        switch (fmtcode & 0xf0) {
        case kLinearLinear: {
            IV c1 = in.Color();
            IV c2 = in.Color();
            DecodeLinearLinear(c1, c2, sub);
            break;
        }
        case kLinearRaw:
            for (int i = 0; i < 8; i++)
                if (needs & (1 << i))
                    sub.colors[i] = in.Color();
            break;
        default:
            Corrupt("invalid linear block format");
        }
        block.FromLinear(axis, sub);
    }

    static void DecodePlanarLinear(const IV& c1, const IV& c2, const IV& c3, const IV& c4, PlanarColorBlock& sub) {
        // a/b run along the first image axis scaled by 7, c along the second
        // scaled by 49, so each step is exact before the final division
        int a1 = c1.x * 7, ad1 = c2.x - c1.x;
        int a2 = c1.y * 7, ad2 = c2.y - c1.y;
        int a3 = c1.z * 7, ad3 = c2.z - c1.z;
        int b1 = c3.x * 7, bd1 = c4.x - c3.x;
        int b2 = c3.y * 7, bd2 = c4.y - c3.y;
        int b3 = c3.z * 7, bd3 = c4.z - c3.z;
        for (int x = 0; x < 8; x++) {
            int e1 = a1 * 7, e2 = a2 * 7, e3 = a3 * 7;
            int ed1 = b1 - a1, ed2 = b2 - a2, ed3 = b3 - a3;
            for (int y = 0; y < 8; y++) {
                sub.colors[x][y] = IV(e1 / 49, e2 / 49, e3 / 49);
                e1 += ed1; e2 += ed2; e3 += ed3;
            }
            a1 += ad1; a2 += ad2; a3 += ad3;
            b1 += bd1; b2 += bd2; b3 += bd3;
        }
    }

    // Tiles are laid out row-major from the bottom of the image, and each
    // tile is upside down, because the reference's JpegWriter writes its
    // bitmap bottom-up while its loader reads top-down.
    void DecodePlanarCT(PlanarColorBlock& sub) {
        uint32_t index = lossyIndex++;
        uint32_t cols = static_cast<uint32_t>(lossy.w) / 8;
        uint32_t rows = static_cast<uint32_t>(lossy.h) / 8;
        uint32_t col = index % cols;
        uint32_t row = index / cols;
        if (row >= rows)
            Corrupt("invalid lossy tile index");
        row = rows - 1 - row;
        size_t stride = static_cast<size_t>(lossy.w) * 3;
        const unsigned char* base = lossy.px + static_cast<size_t>(row) * 8 * stride + static_cast<size_t>(col) * 8 * 3;
        for (int y = 0; y < 8; y++) {
            const unsigned char* p = base + static_cast<size_t>(y) * stride;
            for (int x = 0; x < 8; x++, p += 3)
                sub.colors[x][7 - y] = IV(p[0], p[1], p[2]);
        }
    }

    void DecodePlanar(uint8_t fmtcode, ColorBlock& block) {
        int axis = AxisOf(static_cast<BlockFormat>(fmtcode & 0xf));
        PlanarColorBlock sub;
        uint8_t needs[8];
        PlanarNeeds(block, axis, needs);
        switch (fmtcode & 0xf0) {
        case kPlanarLinear: {
            IV c1 = in.Color();
            IV c2 = in.Color();
            IV c3 = in.Color();
            IV c4 = in.Color();
            DecodePlanarLinear(c1, c2, c3, c4, sub);
            break;
        }
        case kPlanarLossy:
            DecodePlanarCT(sub);
            break;
        case kPlanarRaw:
            for (int x = 0; x < 8; x++)
                for (int yy = 0; yy < 8; yy++) {
                    int y = (x & 1) ? (7 - yy) : yy;
                    if (needs[x] & (1 << y))
                        sub.colors[x][y] = in.Color();
                }
            break;
        default:
            Corrupt("invalid planar block format");
        }
        block.FromPlanar(axis, sub);
    }

    void DecodeVolumetric(uint8_t fmtcode, ColorBlock& block) {
        // the reference defines Volumetric/Linear (0x00) but never implemented it
        if ((fmtcode & 0xf0) != kVolumetricRaw)
            Corrupt("invalid volumetric block format");
        for (int x = 0; x < 8; x++)
            for (int yy = 0; yy < 8; yy++) {
                int y = (x & 1) ? (7 - yy) : yy;
                int b = block.needscolor[x][y];
                if (!b)
                    continue;
                for (int zz = 0; zz < 8; zz++) {
                    int z = (yy & 1) ? (7 - zz) : zz;
                    if (b & (1 << z))
                        block.colors[x][y][z] = IVToColor(in.Color());
                }
            }
    }

public:
    Decoder(Inflater& i, Map& m) : in(i), map(m) {}

    void Decode() {
        if (in.U32() != kSignature)
            Corrupt("bad signature");
        int w = in.U16();
        int h = in.U16();
        int d = in.U16();
        if (w < 1 || h < 1 || d < 1)
            Corrupt("invalid map dimensions");
        CheckDimensions(w, h, d);
        int quality = in.Byte();
        if (quality < 1 || quality > 100)
            Corrupt("invalid quality");

        map.w = w;
        map.h = h;
        map.d = d;
        size_t ncol = static_cast<size_t>(w) * static_cast<size_t>(h);
        map.solid.assign(ncol, 0);
        map.cmask.assign(ncol, 0);
        const uint64_t dmask = map.DepthMask();

        for (int y = 0; y < h; y++) {
            bool odd = (y & 1) != 0;
            for (int i = 0, x = odd ? w - 1 : 0, s = odd ? -1 : 1; i < w; i++, x += s) {
                int fmt = in.Byte();
                uint64_t solid = 0;
                if (static_cast<ColumnFormat>(fmt & 1) == ColumnFormat::Rle) {
                    // each byte is a z where solidity toggles; 0x80 marks a
                    // byte that starts a solid run
                    int z = 0;
                    bool fill = false;
                    while (z < d) {
                        int inb = in.Byte();
                        if (inb & 0x80) {
                            fill = true;
                            inb &= 0x7f;
                        }
                        if (inb <= z || inb > d)
                            Corrupt("invalid z in column");
                        if (fill) {
                            for (; z < inb; z++)
                                solid |= 1ull << z;
                        } else {
                            z = inb;
                        }
                        fill = !fill;
                    }
                } else {
                    for (int z = 0; z < d; z += 16)
                        if ((fmt >> (1 + (z >> 4))) & 1)
                            solid |= static_cast<uint64_t>(in.U16()) << z;
                }
                map.solid[map.Index(x, y)] = solid & dmask;
            }
        }

        // The colour stream skips cells without visible voxels, so the set of
        // coloured voxels is a function of the geometry alone; size the
        // colour store from it before reading any colour.
        uint64_t colouredBlocks = 0;
        ColorBlock block;
        for (int y = 0; y < h; y += 8)
            for (int x = 0; x < w; x += 8)
                for (int zz = 0; zz < d; zz += 8) {
                    int z = CellZ(x, zz, d);
                    ComputeNeedsColor(map, x, y, z, block.needscolor);
                    if (!block.GetNeedsColor())
                        continue;
                    colouredBlocks++;
                    for (int xx = 0; xx < 8 && x + xx < w; xx++)
                        for (int yy = 0; yy < 8 && y + yy < h; yy++) {
                            size_t ci = map.Index(x + xx, y + yy);
                            map.cmask[ci] |= (static_cast<uint64_t>(block.needscolor[xx][yy]) << z) & map.solid[ci];
                        }
                }
        map.AllocateColors();

        uint32_t jpegSize = in.U32();
        // the atlas holds at most one 8x8 tile per coloured cell; a JPEG of
        // pure tiles at q100 is under ~400 bytes each
        uint64_t jpegCap = 65536 + colouredBlocks * 1024;
        if (jpegSize > jpegCap)
            Corrupt("lossy image too large");
        std::vector<uint8_t> jpeg(jpegSize);
        in.Read(jpeg.data(), jpeg.size());
        int iw = 0, ih = 0, comp = 0;
        if (jpeg.empty() || !stbi_info_from_memory(jpeg.data(), static_cast<int>(jpeg.size()), &iw, &ih, &comp))
            Corrupt("lossy image unreadable");
        if (iw < 8 || ih < 8 || (iw & 7) || (ih & 7))
            Corrupt("invalid lossy image dimensions");
        uint64_t tiles = static_cast<uint64_t>(iw / 8) * static_cast<uint64_t>(ih / 8);
        if (tiles > 2 * colouredBlocks + 64)
            Corrupt("lossy image larger than the map needs");
        lossy.px = stbi_load_from_memory(jpeg.data(), static_cast<int>(jpeg.size()), &lossy.w, &lossy.h, &comp, 3);
        if (!lossy.px || lossy.w != iw || lossy.h != ih)
            Corrupt("lossy image undecodable");
        std::vector<uint8_t>().swap(jpeg);

        for (int y = 0; y < h; y += 8) {
            for (int x = 0; x < w; x += 8) {
                for (int zz = 0; zz < d; zz += 8) {
                    int z = CellZ(x, zz, d);
                    ColorBlock cb;
                    ComputeNeedsColor(map, x, y, z, cb.needscolor);
                    if (!cb.GetNeedsColor())
                        continue;
                    uint8_t fmtcode = in.Byte();
                    switch (static_cast<BlockFormat>(fmtcode & 0xf)) {
                    case BlockFormat::Constant:
                        // the reference ignores the sub-format bits here
                        cb.FromConstant(in.Color());
                        break;
                    case BlockFormat::LinearX:
                    case BlockFormat::LinearY:
                    case BlockFormat::LinearZ:
                        DecodeLinear(fmtcode, cb);
                        break;
                    case BlockFormat::PlanarX:
                    case BlockFormat::PlanarY:
                    case BlockFormat::PlanarZ:
                        DecodePlanar(fmtcode, cb);
                        break;
                    case BlockFormat::Volumetric:
                        DecodeVolumetric(fmtcode, cb);
                        break;
                    default:
                        Corrupt("unsupported block format");
                    }
                    // SetColor drops voxels outside the map or not solid
                    cb.ForEach([&](int xx, int yy, int zc) {
                        map.SetColor(x + xx, y + yy, z + zc, cb.colors[xx][yy][zc]);
                    });
                }
            }
        }
    }
};

// ---- encoder ----

class Emitter {
    std::vector<uint8_t>& out;
    int quality;
    int diversityLimit;

    struct JpegBlock {
        IV colors[8][8];
    };
    std::vector<JpegBlock> jpegBlocks;

    void Color(const IV& c) {
        out.push_back(static_cast<uint8_t>(c.x));
        out.push_back(static_cast<uint8_t>(c.y));
        out.push_back(static_cast<uint8_t>(c.z));
    }

    void EncodeVolumetricRaw(const ColorBlock& block) {
        out.push_back(MakeFormat(BlockFormat::Volumetric, kVolumetricRaw));
        for (int x = 0; x < 8; x++)
            for (int yy = 0; yy < 8; yy++) {
                int y = (x & 1) ? (7 - yy) : yy;
                int b = block.needscolor[x][y];
                if (!b)
                    continue;
                for (int zz = 0; zz < 8; zz++) {
                    int z = (yy & 1) ? (7 - zz) : zz;
                    if (b & (1 << z))
                        Color(block.At(x, y, z));
                }
            }
    }

    // Copies the nearest needed pixel into the unused ones so the JPEG tile
    // has no artificial edges.
    static void FillGap(PlanarColorBlock& sub, const uint8_t needscolor[8]) {
        bool full = true;
        for (int i = 0; i < 8; i++)
            full = full && needscolor[i] == 0xff;
        if (full)
            return;

        struct Point {
            int8_t x, y;
            static Point Bad() { return Point{-1, -1}; }
            bool IsBad() const { return x == -1; }
            int DistanceTo(Point o) const {
                int xx = x - o.x, yy = y - o.y;
                // the reference wrote "xx = -yy" here, a typo that only
                // made the fill slightly worse
                if (xx < 0)
                    xx = -xx;
                if (yy < 0)
                    yy = -yy;
                return xx + yy;
            }
            static Point Nearer(Point here, Point a, Point b) {
                if (a.IsBad())
                    return b;
                if (b.IsBad())
                    return a;
                return here.DistanceTo(a) < here.DistanceTo(b) ? a : b;
            }
        };
        auto P = [](int x, int y) { return Point{static_cast<int8_t>(x), static_cast<int8_t>(y)}; };
        auto needs = [&](int x, int y) { return (needscolor[x] & (1 << y)) != 0; };

        Point nearest[8][8];
        Point tmpnear[8], oldnear[8];
        for (auto& row : nearest)
            std::fill(row, row + 8, Point::Bad());

        // four sweeps (+x, -x, +y, -y), each propagating the nearest needed
        // pixel from the previous line
        for (int pass = 0; pass < 4; pass++) {
            std::fill(tmpnear, tmpnear + 8, Point::Bad());
            for (int step = 0; step < 8; step++) {
                std::copy(tmpnear, tmpnear + 8, oldnear);
                for (int i = 0; i < 8; i++) {
                    int x, y;
                    if (pass == 0) { x = step; y = i; }
                    else if (pass == 1) { x = 7 - step; y = i; }
                    else if (pass == 2) { x = i; y = step; }
                    else { x = i; y = 7 - step; }
                    Point here = P(x, y);
                    if (needs(x, y)) {
                        tmpnear[i] = here;
                    } else if (i == 0) {
                        tmpnear[i] = Point::Nearer(here, oldnear[i + 1], oldnear[i]);
                    } else if (i == 7) {
                        tmpnear[i] = Point::Nearer(here, oldnear[i - 1], oldnear[i]);
                    } else {
                        tmpnear[i] = Point::Nearer(here, Point::Nearer(here, oldnear[i - 1], oldnear[i + 1]), oldnear[i]);
                    }
                    nearest[x][y] = pass == 0 ? tmpnear[i] : Point::Nearer(here, tmpnear[i], nearest[x][y]);
                }
            }
        }
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++) {
                if (needs(x, y))
                    continue;
                Point p = nearest[x][y];
                if (!p.IsBad())
                    sub.colors[x][y] = sub.colors[p.x][p.y];
            }
    }

    static int CountNeedsColor(const uint8_t* bits, size_t numRows) {
        int cnt = 0;
        for (size_t i = 0; i < numRows; i++)
            cnt += __builtin_popcount(bits[i]);
        return cnt;
    }

    // Distinct colours among the needed pixels, stopping at `limit`.
    static int CountColors(const PlanarColorBlock& sub, int limit, const uint8_t* needscolor) {
        IV seen[64];
        int count = 0;
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++) {
                if (!(needscolor[x] & (1 << y)))
                    continue;
                const IV& c = sub.colors[x][y];
                bool found = false;
                for (int i = 0; i < count && !found; i++)
                    found = seen[i] == c;
                if (found)
                    continue;
                seen[count++] = c;
                if (count >= limit)
                    return count;
            }
        return count;
    }

    void EncodePlanar(PlanarColorBlock& sub, const ColorBlock& block, BlockFormat fmt) {
        uint8_t needs[8];
        PlanarNeeds(block, AxisOf(fmt), needs);
        // an image with few colours compresses better with deflate alone
        if (quality < 100 && CountNeedsColor(needs, 8) > 5 && CountColors(sub, 16, needs) >= 14) {
            FillGap(sub, needs);
            out.push_back(MakeFormat(fmt, kPlanarLossy));
            JpegBlock jb;
            for (int x = 0; x < 8; x++)
                for (int y = 0; y < 8; y++)
                    jb.colors[x][y] = sub.colors[x][y];
            jpegBlocks.push_back(jb);
        } else {
            out.push_back(MakeFormat(fmt, kPlanarRaw));
            for (int x = 0; x < 8; x++)
                for (int yy = 0; yy < 8; yy++) {
                    int y = (x & 1) ? (7 - yy) : yy;
                    if (needs[x] & (1 << y))
                        Color(sub.colors[x][y]);
                }
        }
    }

    void TryEmitPlanar(const ColorBlock& block) {
        int xx = block.ComputeDiversity2D(0);
        int yy = block.ComputeDiversity2D(1);
        int zz = block.ComputeDiversity2D(2);
        int mn = std::min(xx, std::min(yy, zz));
        if (mn > diversityLimit) {
            EncodeVolumetricRaw(block);
            return;
        }
        BlockFormat fmt = xx == mn ? BlockFormat::PlanarX : yy == mn ? BlockFormat::PlanarY : BlockFormat::PlanarZ;
        PlanarColorBlock sub;
        block.ToPlanar(AxisOf(fmt), sub);
        EncodePlanar(sub, block, fmt);
    }

    static IV Clamp(IV c) {
        c.x = std::min(255, std::max(0, c.x));
        c.y = std::min(255, std::max(0, c.y));
        c.z = std::min(255, std::max(0, c.z));
        return c;
    }

    void EncodeLinear(const LinearColorBlock& sub, const ColorBlock& block, BlockFormat fmt) {
        uint8_t needs = LinearNeeds(block, AxisOf(fmt));

        // least squares fit of a gradient over the needed entries
        IV avg;
        int numcolors = 0;
        for (int i = 0; i < 8; i++)
            if (needs & (1 << i)) {
                avg += sub.colors[i];
                numcolors++;
            }
        if (numcolors > 1)
            avg /= numcolors;
        static const int weights[8] = {-7, -5, -3, -1, 1, 3, 5, 7};
        IV coef;
        int factor = 0;
        for (int i = 0; i < 8; i++)
            if (needs & (1 << i)) {
                coef += (sub.colors[i] - avg) * weights[i];
                factor += weights[i] * weights[i];
            }
        coef *= 7;
        if (factor > 0)
            coef /= factor;
        IV col1 = Clamp(avg - coef);
        IV col2 = Clamp(avg + coef);

        LinearColorBlock pred;
        Decoder::DecodeLinearLinear(col1, col2, pred);
        int error = 0;
        for (int i = 0; i < 8; i++)
            if (needs & (1 << i)) {
                IV diff = sub.colors[i] - pred.colors[i];
                error = std::max(error, IV::Dot(diff, diff));
            }

        if (error < diversityLimit) {
            out.push_back(MakeFormat(fmt, kLinearLinear));
            Color(col1);
            Color(col2);
        } else {
            out.push_back(MakeFormat(fmt, kLinearRaw));
            for (int i = 0; i < 8; i++)
                if (needs & (1 << i))
                    Color(sub.colors[i]);
        }
    }

    void TryEmitLinear(const ColorBlock& block) {
        int xx = block.ComputeDiversity1D(0);
        int yy = block.ComputeDiversity1D(1);
        int zz = block.ComputeDiversity1D(2);
        int mn = std::min(xx, std::min(yy, zz));
        if (mn * 5 > diversityLimit) {
            TryEmitPlanar(block);
            return;
        }
        BlockFormat fmt = xx == mn ? BlockFormat::LinearX : yy == mn ? BlockFormat::LinearY : BlockFormat::LinearZ;
        LinearColorBlock sub;
        block.ToLinear(AxisOf(fmt), sub);
        EncodeLinear(sub, block, fmt);
    }

public:
    Emitter(std::vector<uint8_t>& o, int q) : out(o), quality(q) {
        diversityLimit = 34 - q / 3;
        diversityLimit *= diversityLimit;
    }

    // Cheapest model first; each falls back to the next when the colours
    // vary more than the quality allows.
    void TryEmitConstant(const ColorBlock& block) {
        // constant mode looks bad, hence the stricter threshold
        if (block.ComputeDiversity() * 6 <= diversityLimit) {
            out.push_back(MakeFormat(BlockFormat::Constant, 0));
            Color(block.ToConstant());
        } else {
            TryEmitLinear(block);
        }
    }

    // The atlas is written bottom-up, like the reference's JpegWriter, so
    // the decoder's flips undo it (see DecodePlanarCT).
    std::vector<uint8_t> EncodeJpeg() const {
        unsigned numBlocks = static_cast<unsigned>(jpegBlocks.size());
        if (numBlocks == 0)
            numBlocks = 1;
        unsigned cols = static_cast<unsigned>(std::ceil(std::sqrt(static_cast<double>(numBlocks))));
        unsigned rows = (numBlocks + cols - 1) / cols;
        unsigned iw = cols * 8, ih = rows * 8;
        std::vector<uint8_t> rgb(static_cast<size_t>(iw) * ih * 3, 0);
        unsigned col = 0, row = 0;
        for (const auto& b : jpegBlocks) {
            for (unsigned y = 0; y < 8; y++) {
                // bitmap row (row * 8 + y) is JPEG scanline ih - 1 - that
                size_t line = ih - 1 - (row * 8 + y);
                uint8_t* p = rgb.data() + (line * iw + col * 8) * 3;
                for (unsigned x = 0; x < 8; x++) {
                    const IV& c = b.colors[x][y];
                    *p++ = static_cast<uint8_t>(c.x);
                    *p++ = static_cast<uint8_t>(c.y);
                    *p++ = static_cast<uint8_t>(c.z);
                }
            }
            if (++col == cols) {
                col = 0;
                ++row;
            }
        }

        struct Sink : jpge::output_stream {
            std::vector<uint8_t> data;
            bool put_buf(const void* buf, int len) override {
                const uint8_t* b = static_cast<const uint8_t*>(buf);
                data.insert(data.end(), b, b + len);
                return true;
            }
        } sink;
        jpge::params params;
        params.m_quality = quality;
        jpge::jpeg_encoder enc;
        if (!enc.init(&sink, static_cast<int>(iw), static_cast<int>(ih), 3, params))
            throw Error(NGMAP_ERR_INTERNAL, "jpeg encoder init failed");
        for (jpge::uint pass = 0; pass < enc.get_total_passes(); pass++) {
            for (unsigned y = 0; y < ih; y++)
                if (!enc.process_scanline(rgb.data() + static_cast<size_t>(y) * iw * 3))
                    throw Error(NGMAP_ERR_INTERNAL, "jpeg encoding failed");
            if (!enc.process_scanline(nullptr))
                throw Error(NGMAP_ERR_INTERNAL, "jpeg encoding failed");
        }
        enc.deinit();
        return std::move(sink.data);
    }
};

static void PutU16(std::vector<uint8_t>& o, unsigned v) {
    o.push_back(static_cast<uint8_t>(v));
    o.push_back(static_cast<uint8_t>(v >> 8));
}
static void PutU32(std::vector<uint8_t>& o, uint32_t v) {
    PutU16(o, v & 0xffff);
    PutU16(o, v >> 16);
}

static std::vector<uint8_t> Encode(const Map& map, int quality) {
    const int w = map.w, h = map.h, d = map.d;
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(w) * static_cast<size_t>(h) * 4 + 1024);
    PutU32(raw, kSignature);
    PutU16(raw, static_cast<unsigned>(w));
    PutU16(raw, static_cast<unsigned>(h));
    PutU16(raw, static_cast<unsigned>(d));
    raw.push_back(static_cast<uint8_t>(quality));

    std::vector<uint8_t> rle;
    for (int y = 0; y < h; y++) {
        bool odd = (y & 1) != 0;
        for (int i = 0, x = odd ? w - 1 : 0, s = odd ? -1 : 1; i < w; i++, x += s) {
            uint64_t solid = map.Solid(x, y);
            // RLE: the z of every toggle, then d; the first byte carries
            // 0x80 when that byte starts a solid run
            rle.clear();
            int z = 1;
            bool last = (solid & 1) != 0;
            bool first = true;
            for (; z < d; z++) {
                bool b = ((solid >> z) & 1) != 0;
                if (b != last) {
                    uint8_t outb = static_cast<uint8_t>(z);
                    if (first) {
                        first = false;
                        if (last)
                            outb |= 0x80;
                    }
                    rle.push_back(outb);
                    last = b;
                }
            }
            {
                uint8_t outb = static_cast<uint8_t>(z);
                if (first && last)
                    outb |= 0x80;
                rle.push_back(outb);
            }

            size_t bmpSize = 0;
            int bmpFmt = static_cast<int>(ColumnFormat::Bitmap);
            for (int zz = 0; zz < d; zz += 16)
                if ((solid >> zz) & 0xffff) {
                    bmpSize += 2;
                    bmpFmt |= 1 << (1 + (zz >> 4));
                }
            if (bmpSize < rle.size() && bmpSize != 0) {
                raw.push_back(static_cast<uint8_t>(bmpFmt));
                for (int zz = 0; zz < d; zz += 16)
                    if ((solid >> zz) & 0xffff)
                        PutU16(raw, static_cast<unsigned>((solid >> zz) & 0xffff));
            } else {
                raw.push_back(static_cast<uint8_t>(ColumnFormat::Rle));
                raw.insert(raw.end(), rle.begin(), rle.end());
            }
        }
    }

    std::vector<uint8_t> blocks;
    blocks.reserve(4 * 1024 * 1024);
    Emitter emitter(blocks, quality);
    for (int y = 0; y < h; y += 8)
        for (int x = 0; x < w; x += 8)
            for (int zz = 0; zz < d; zz += 8) {
                int z = CellZ(x, zz, d);
                ColorBlock block;
                ComputeNeedsColor(map, x, y, z, block.needscolor);
                if (!block.GetNeedsColor())
                    continue;
                block.ForEach([&](int bx, int by, int bz) {
                    block.colors[bx][by][bz] = map.GetColor(x + bx, y + by, z + bz);
                });
                emitter.TryEmitConstant(block);
            }

    std::vector<uint8_t> jpeg = emitter.EncodeJpeg();
    PutU32(raw, static_cast<uint32_t>(jpeg.size()));
    raw.insert(raw.end(), jpeg.begin(), jpeg.end());
    raw.insert(raw.end(), blocks.begin(), blocks.end());

    // the format is the zlib wrapper (reference: deflateInit level 5); any
    // level decodes the same, 9 only costs encoder time
    uLongf zsize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> z(zsize);
    if (compress2(z.data(), &zsize, raw.data(), static_cast<uLong>(raw.size()), 9) != Z_OK)
        throw Error(NGMAP_ERR_INTERNAL, "deflate failed");
    z.resize(zsize);
    return z;
}

static uint8_t* ToMalloc(const std::vector<uint8_t>& v) {
    uint8_t* p = static_cast<uint8_t*>(std::malloc(v.empty() ? 1 : v.size()));
    if (!p)
        throw std::bad_alloc();
    if (!v.empty())
        std::memcpy(p, v.data(), v.size());
    return p;
}

} // namespace ngmap

using namespace ngmap;

extern "C" int ngmap_encode(const uint8_t* vxl, size_t vxl_size, int width, int height, int depth, int quality,
                            uint8_t** out, size_t* out_size) {
    if (!vxl || !out || !out_size || quality < 1 || quality > 100)
        return NGMAP_ERR_ARGS;
    *out = nullptr;
    *out_size = 0;
    try {
        Map map;
        ParseVxl(vxl, vxl_size, width, height, depth, map);
        std::vector<uint8_t> data = Encode(map, quality);
        *out = ToMalloc(data);
        *out_size = data.size();
        return NGMAP_OK;
    } catch (const Error& e) {
        return e.code;
    } catch (const std::bad_alloc&) {
        return NGMAP_ERR_NOMEM;
    } catch (...) {
        return NGMAP_ERR_INTERNAL;
    }
}

extern "C" int ngmap_decode(const uint8_t* data, size_t size, uint8_t** vxl_out, size_t* vxl_size, int* width,
                            int* height, int* depth) {
    if (!data || !vxl_out || !vxl_size || !width || !height || !depth)
        return NGMAP_ERR_ARGS;
    *vxl_out = nullptr;
    *vxl_size = 0;
    try {
        Map map;
        {
            Inflater in(data, size);
            Decoder(in, map).Decode();
        }
        std::vector<uint8_t> vxl = WriteVxl(map);
        *vxl_out = ToMalloc(vxl);
        *vxl_size = vxl.size();
        *width = map.w;
        *height = map.h;
        *depth = map.d;
        return NGMAP_OK;
    } catch (const Error& e) {
        return e.code;
    } catch (const std::bad_alloc&) {
        return NGMAP_ERR_NOMEM;
    } catch (...) {
        return NGMAP_ERR_INTERNAL;
    }
}

extern "C" int ngmap_decode_columns(const uint8_t* data, size_t size, ngmap_size_fn size_fn,
                                    ngmap_column_fn column_fn, void* user) {
    if (!data || !size_fn || !column_fn)
        return NGMAP_ERR_ARGS;
    try {
        Map map;
        {
            Inflater in(data, size);
            Decoder(in, map).Decode();
        }
        if (size_fn(user, map.w, map.h, map.d) != 0)
            return NGMAP_ERR_ARGS;
        uint32_t colors[64];
        for (int y = 0; y < map.h; y++) {
            for (int x = 0; x < map.w; x++) {
                // the colours WriteVxl gives the column, as its bytes read
                uint64_t s = map.Solid(x, y) & map.DepthMask();
                uint64_t c = (map.cmask[map.Index(x, y)] & s) | (s & ~(s << 1)) | (s & ~(s >> 1));
                int n = 0;
                for (uint64_t left = c; left; left &= left - 1) {
                    uint32_t v = map.GetColor(x, y, __builtin_ctzll(left));
                    colors[n++] = 0x7f000000u | (v & 0xffu) << 16 | (v & 0xff00u) | (v >> 16 & 0xffu);
                }
                column_fn(user, x, y, s, c, colors);
            }
        }
        return NGMAP_OK;
    } catch (const Error& e) {
        return e.code;
    } catch (const std::bad_alloc&) {
        return NGMAP_ERR_NOMEM;
    } catch (...) {
        return NGMAP_ERR_INTERNAL;
    }
}

extern "C" const char* ngmap_strerror(int err) {
    switch (err) {
    case NGMAP_OK: return "ok";
    case NGMAP_ERR_ARGS: return "invalid argument";
    case NGMAP_ERR_VXL: return "malformed vxl";
    case NGMAP_ERR_CORRUPT: return "malformed ngmap stream";
    case NGMAP_ERR_TOO_LARGE: return "map too large";
    case NGMAP_ERR_NOMEM: return "out of memory";
    case NGMAP_ERR_INTERNAL: return "internal error";
    default: return "unknown error";
    }
}
