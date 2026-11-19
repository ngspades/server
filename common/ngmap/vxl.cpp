// AoS VXL reader/writer for the ngmap codec.
//
// VXL column layout (per column, spans until N == 0):
//   u8 N  (span length in 4-byte units, 0 = last span)
//   u8 S  (first solid z of the top colour run)
//   u8 E  (last z of the top colour run, inclusive; E = S - 1 means none)
//   u8 A  (z where this span's air starts)
//   u32 colours[E - S + 1] then, unless last, the bottom colour run that ends
//   right above the next span's A. Solid voxels between the two runs carry no
//   colour; everything below the last span's top run is solid.
#include "vxl.h"

#include <cstring>

namespace ngmap {

static int Popcount(uint64_t v) { return __builtin_popcountll(v); }

void CheckDimensions(int w, int h, int d) {
    if (w < 1 || h < 1 || d < 1 || w > kMaxWidth || h > kMaxHeight || d > kMaxDepth)
        throw Error(NGMAP_ERR_TOO_LARGE, "map dimensions out of range");
}

uint32_t Map::GetColor(int x, int y, int z) const {
    if (x < 0 || y < 0 || z < 0 || x >= w || y >= h || z >= d)
        return kDefaultColor;
    size_t i = Index(x, y);
    uint64_t bit = 1ull << z;
    if (!(cmask[i] & bit))
        return kDefaultColor;
    return colors[coff[i] + static_cast<uint32_t>(Popcount(cmask[i] & (bit - 1)))];
}

void Map::SetColor(int x, int y, int z, uint32_t c) {
    if (x < 0 || y < 0 || z < 0 || x >= w || y >= h || z >= d)
        return;
    size_t i = Index(x, y);
    uint64_t bit = 1ull << z;
    if (!(cmask[i] & bit))
        return;
    colors[coff[i] + static_cast<uint32_t>(Popcount(cmask[i] & (bit - 1)))] = c & 0xffffffu;
}

void Map::AllocateColors() {
    size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    coff.assign(n + 1, 0);
    uint64_t total = 0;
    for (size_t i = 0; i < n; i++) {
        coff[i] = static_cast<uint32_t>(total);
        total += static_cast<uint64_t>(Popcount(cmask[i]));
        if (total > kMaxColors)
            throw Error(NGMAP_ERR_TOO_LARGE, "too many coloured voxels");
    }
    coff[n] = static_cast<uint32_t>(total);
    colors.assign(static_cast<size_t>(total), kDefaultColor);
}

void ParseVxl(const uint8_t* v, size_t size, int w, int h, int d, Map& m) {
    CheckDimensions(w, h, d);
    m.w = w;
    m.h = h;
    m.d = d;
    size_t ncol = static_cast<size_t>(w) * static_cast<size_t>(h);
    m.solid.assign(ncol, 0);
    m.cmask.assign(ncol, 0);
    m.coff.assign(ncol + 1, 0);
    m.colors.clear();
    const uint64_t dmask = m.DepthMask();

    size_t pos = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t ci = m.Index(x, y);
            m.coff[ci] = static_cast<uint32_t>(m.colors.size());
            uint64_t solid = dmask;
            uint64_t cmask = 0;
            int z = 0;
            for (;;) {
                if (size - pos < 4)
                    throw Error(NGMAP_ERR_VXL, "vxl truncated");
                int n = v[pos], s = v[pos + 1], e = v[pos + 2];
                // the span's own A was consumed by the previous span; only
                // z (where the air starts) matters here
                if (s < z || s > d || e < s - 1 || e >= d)
                    throw Error(NGMAP_ERR_VXL, "vxl span out of range");
                for (int i = z; i < s; i++)
                    solid &= ~(1ull << i);
                int lenTop = e - s + 1;
                if ((size - pos - 4) / 4 < static_cast<size_t>(lenTop))
                    throw Error(NGMAP_ERR_VXL, "vxl truncated");
                const uint8_t* col = v + pos + 4;
                for (int i = 0; i < lenTop; i++, col += 4) {
                    // VXL stores B, G, R, A
                    m.colors.push_back(static_cast<uint32_t>(col[2]) | static_cast<uint32_t>(col[1]) << 8 |
                                       static_cast<uint32_t>(col[0]) << 16);
                    cmask |= 1ull << (s + i);
                }
                if (n == 0) {
                    pos += 4 * static_cast<size_t>(lenTop + 1);
                    break;
                }
                int lenBottom = n - 1 - lenTop;
                if (lenBottom < 0)
                    throw Error(NGMAP_ERR_VXL, "vxl span length inconsistent");
                if ((size - pos) / 4 < static_cast<size_t>(n) + 1)
                    throw Error(NGMAP_ERR_VXL, "vxl truncated");
                pos += 4 * static_cast<size_t>(n);
                int a = v[pos + 3];
                int bottomStart = a - lenBottom;
                if (a > d || bottomStart <= e || a < s)
                    throw Error(NGMAP_ERR_VXL, "vxl bottom run out of range");
                for (int i = 0; i < lenBottom; i++, col += 4) {
                    m.colors.push_back(static_cast<uint32_t>(col[2]) | static_cast<uint32_t>(col[1]) << 8 |
                                       static_cast<uint32_t>(col[0]) << 16);
                    cmask |= 1ull << (bottomStart + i);
                }
                z = a;
            }
            if (m.colors.size() > kMaxColors)
                throw Error(NGMAP_ERR_TOO_LARGE, "too many coloured voxels");
            m.solid[ci] = solid;
            m.cmask[ci] = cmask & solid;
        }
    }
    m.coff[ncol] = static_cast<uint32_t>(m.colors.size());
}

static void PutColor(std::vector<uint8_t>& out, uint32_t c) {
    out.push_back(static_cast<uint8_t>(c >> 16)); // B
    out.push_back(static_cast<uint8_t>(c >> 8));  // G
    out.push_back(static_cast<uint8_t>(c));       // R
    out.push_back(0x7f);                          // unused by the games
}

// Same span layout as pysnip's and OpenSpades' GameMap::Save, with "surface"
// meaning "has a colour". Coloured voxels in the middle of a solid run become
// spans with no air, which every VXL loader accepts since pysnip writes them.
std::vector<uint8_t> WriteVxl(const Map& m) {
    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(m.w) * static_cast<size_t>(m.h) * 12);
    const int d = m.d;
    for (int y = 0; y < m.h; y++) {
        for (int x = 0; x < m.w; x++) {
            uint64_t s = m.Solid(x, y) & m.DepthMask();
            uint64_t tops = s & ~(s << 1);
            uint64_t bottoms = s & ~(s >> 1);
            uint64_t c = (m.cmask[m.Index(x, y)] & s) | tops | bottoms;
            auto solid = [&](int z) { return ((s >> z) & 1) != 0; };
            auto surf = [&](int z) { return z < d && ((c >> z) & 1) != 0; };

            int k = 0;
            while (k < d) {
                int airStart = k;
                while (k < d && !solid(k))
                    ++k;
                int topStart = k;
                while (k < d && surf(k))
                    ++k;
                int topEnd = k;
                while (k < d && solid(k) && !surf(k))
                    ++k;
                int bottomStart = k;
                int z = k;
                while (z < d && surf(z))
                    ++z;
                if (z != d) {
                    while (surf(k))
                        ++k;
                }
                int bottomEnd = k;
                int topLen = topEnd - topStart;
                int bottomLen = bottomEnd - bottomStart;
                out.push_back(k == d ? 0 : static_cast<uint8_t>(topLen + bottomLen + 1));
                out.push_back(static_cast<uint8_t>(topStart));
                out.push_back(static_cast<uint8_t>(topEnd - 1));
                out.push_back(static_cast<uint8_t>(airStart));
                for (int i = 0; i < topLen; i++)
                    PutColor(out, m.GetColor(x, y, topStart + i));
                for (int i = 0; i < bottomLen; i++)
                    PutColor(out, m.GetColor(x, y, bottomStart + i));
            }
        }
    }
    return out;
}

} // namespace ngmap
