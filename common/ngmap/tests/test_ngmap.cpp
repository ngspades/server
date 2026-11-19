// ngmap codec tests: round trips on the real maps and malformed input.
//
//   test_ngmap [maps_dir]
//
// maps_dir holds <Name>/<Name>.vxl (512x512x64 AoS maps); it defaults to the
// main checkout's server/Resources/maps. Build with the sanitizers (see
// CMakeLists.txt): the malformed-input half only proves something there.
#include "../ngmap.h"
#include "../vxl.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <zlib.h>

#ifndef NGMAP_DEFAULT_MAPS
/* from the repository root, where `make test-ngmap` runs it */
#define NGMAP_DEFAULT_MAPS "server/Resources/maps"
#endif

extern "C" int ngmap_c_api_check(void);

namespace fs = std::filesystem;
using ngmap::Map;

static int failures = 0;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                                \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static double Ms(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
}

static std::vector<uint8_t> ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::vector<uint8_t> Deflate(const std::vector<uint8_t>& in, int level) {
    uLongf n = compressBound(static_cast<uLong>(in.size()));
    std::vector<uint8_t> out(n);
    if (compress2(out.data(), &n, in.data(), static_cast<uLong>(in.size()), level) != Z_OK)
        return {};
    out.resize(n);
    return out;
}

static std::vector<uint8_t> Inflate(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out;
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (inflateInit(&zs) != Z_OK)
        return out;
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());
    uint8_t buf[65536];
    int ret;
    do {
        zs.next_out = buf;
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        out.insert(out.end(), buf, buf + (sizeof(buf) - zs.avail_out));
    } while (ret == Z_OK);
    inflateEnd(&zs);
    return out;
}

// Visible = solid with an empty neighbour, by the rule the VXL writers
// (pysnip, OpenSpades GameMap::IsSurface) use: the map edge is not air and
// the bottom face is never visible. These are the voxels whose colour a
// player can see, so these are the ones the codec must carry.
static uint64_t VisibleMask(const Map& m, int x, int y) {
    uint64_t s = m.Solid(x, y);
    uint64_t air = ~(s << 1); // voxel above is air; z = 0 counts as exposed
    uint64_t below = ~(s >> 1) & (m.DepthMask() >> 1);
    uint64_t v = air | below;
    if (x > 0) v |= ~m.Solid(x - 1, y);
    if (x < m.w - 1) v |= ~m.Solid(x + 1, y);
    if (y > 0) v |= ~m.Solid(x, y - 1);
    if (y < m.h - 1) v |= ~m.Solid(x, y + 1);
    return s & v;
}

struct ColorStats {
    uint64_t compared = 0, missing = 0, exact = 0;
    double sum[3] = {0, 0, 0};
    int max[3] = {0, 0, 0};
};

static bool SameGeometry(const Map& a, const Map& b, int* firstBad) {
    if (a.w != b.w || a.h != b.h || a.d != b.d)
        return false;
    for (size_t i = 0; i < a.solid.size(); i++)
        if (a.solid[i] != b.solid[i]) {
            *firstBad = static_cast<int>(i);
            return false;
        }
    return true;
}

static ColorStats CompareColors(const Map& orig, const Map& dec) {
    ColorStats st;
    for (int y = 0; y < orig.h; y++)
        for (int x = 0; x < orig.w; x++) {
            uint64_t vis = VisibleMask(orig, x, y);
            uint64_t have = dec.cmask[dec.Index(x, y)];
            for (int z = 0; z < orig.d; z++) {
                if (!((vis >> z) & 1))
                    continue;
                if (!((have >> z) & 1)) {
                    st.missing++;
                    continue;
                }
                uint32_t a = orig.GetColor(x, y, z), b = dec.GetColor(x, y, z);
                st.compared++;
                if (a == b)
                    st.exact++;
                for (int c = 0; c < 3; c++) {
                    int e = std::abs(static_cast<int>((a >> (8 * c)) & 0xff) - static_cast<int>((b >> (8 * c)) & 0xff));
                    st.sum[c] += e;
                    st.max[c] = std::max(st.max[c], e);
                }
            }
        }
    return st;
}

static bool ParseOk(const uint8_t* v, size_t n, int w, int h, int d, Map& m) {
    try {
        ngmap::ParseVxl(v, n, w, h, d, m);
        return true;
    } catch (const ngmap::Error& e) {
        std::printf("  parse error: %s\n", e.what());
        return false;
    }
}

// ---- round trips on real maps ----

static void TestMap(const fs::path& path) {
    const int W = 512, H = 512, D = 64;
    std::vector<uint8_t> vxl = ReadFile(path);
    std::string name = path.stem().string();
    Map orig;
    if (!ParseOk(vxl.data(), vxl.size(), W, H, D, orig)) {
        CHECK(false, "%s does not parse", name.c_str());
        return;
    }

    // our own VXL writer must reproduce the map exactly (it is what the
    // decoder outputs)
    {
        std::vector<uint8_t> again = ngmap::WriteVxl(orig);
        Map back;
        int bad = -1;
        CHECK(ParseOk(again.data(), again.size(), W, H, D, back) && SameGeometry(orig, back, &bad),
              "%s: vxl writer changed the geometry (column %d)", name.c_str(), bad);
        ColorStats st = CompareColors(orig, back);
        CHECK(st.missing == 0 && st.exact == st.compared, "%s: vxl writer changed colours", name.c_str());
    }

    size_t zsize = Deflate(vxl, 9).size();
    std::printf("%-18s vxl %8zu  zlib9 %7zu\n", name.c_str(), vxl.size(), zsize);

    for (int q : {100, 80, 50}) {
        auto t0 = std::chrono::steady_clock::now();
        uint8_t* ng = nullptr;
        size_t ngSize = 0;
        int r = ngmap_encode(vxl.data(), vxl.size(), W, H, D, q, &ng, &ngSize);
        double encMs = Ms(t0);
        CHECK(r == NGMAP_OK, "%s q%d encode: %s", name.c_str(), q, ngmap_strerror(r));
        if (r != NGMAP_OK)
            continue;
        t0 = std::chrono::steady_clock::now();
        uint8_t* out = nullptr;
        size_t outSize = 0;
        int w = 0, h = 0, d = 0;
        r = ngmap_decode(ng, ngSize, &out, &outSize, &w, &h, &d);
        double decMs = Ms(t0);
        std::free(ng);
        CHECK(r == NGMAP_OK && w == W && h == H && d == D, "%s q%d decode: %s", name.c_str(), q, ngmap_strerror(r));
        if (r != NGMAP_OK) {
            std::free(out);
            continue;
        }
        Map dec;
        bool parsed = ParseOk(out, outSize, w, h, d, dec);
        std::free(out);
        CHECK(parsed, "%s q%d: decoded vxl does not parse", name.c_str(), q);
        if (!parsed)
            continue;
        int bad = -1;
        CHECK(SameGeometry(orig, dec, &bad), "%s q%d: geometry differs (column %d)", name.c_str(), q, bad);
        ColorStats st = CompareColors(orig, dec);
        CHECK(st.missing == 0, "%s q%d: %llu visible voxels lost their colour", name.c_str(), q,
              static_cast<unsigned long long>(st.missing));
        double n = st.compared ? static_cast<double>(st.compared) : 1.0;
        std::printf("  q%-3d ngmap %7zu (%5.1f%% of zlib9)  enc %6.0f ms dec %6.0f ms  visible %8llu exact %5.1f%%  "
                    "mean err R %.2f G %.2f B %.2f  max R %d G %d B %d\n",
                    q, ngSize, 100.0 * static_cast<double>(ngSize) / static_cast<double>(zsize), encMs, decMs,
                    static_cast<unsigned long long>(st.compared), 100.0 * static_cast<double>(st.exact) / n,
                    st.sum[0] / n, st.sum[1] / n, st.sum[2] / n, st.max[0], st.max[1], st.max[2]);
        if (q == 100)
            CHECK(st.max[0] <= 1 && st.max[1] <= 1 && st.max[2] <= 1, "%s q100: colour error above 1", name.c_str());
    }
}

// ---- synthetic map for the fuzz half (small so thousands of decodes run
// quickly under ASan, varied so every block format appears) ----

static std::vector<uint8_t> SyntheticVxl(int W, int H, int D, uint32_t seed) {
    std::mt19937 rng(seed);
    Map m;
    m.w = W;
    m.h = H;
    m.d = D;
    size_t n = static_cast<size_t>(W) * H;
    m.solid.assign(n, 0);
    m.cmask.assign(n, 0);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int top = 20 + static_cast<int>(10 * std::sin(x * 0.2) + 8 * std::cos(y * 0.15));
            uint64_t s = ~0ull << top;
            if ((x / 8 + y / 8) % 3 == 0)
                s &= ~(0xffull << 40); // a cave layer
            if (x % 13 == 0 && y % 11 < 3 && top >= 10)
                s |= 0xfull << (top - 8); // floating bits
            if (x % 17 == 0 && y % 5 == 0)
                s = 0; // all-air column, like Empty_ocean has
            if (x % 19 == 0 && y % 7 == 0)
                s = 0xffull << 20; // column ending in air
            m.solid[m.Index(x, y)] = s & m.DepthMask();
            m.cmask[m.Index(x, y)] = m.solid[m.Index(x, y)];
        }
    m.AllocateColors();
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            for (int z = 0; z < D; z++) {
                uint32_t c;
                int region = (x / 16 + 2 * (y / 16)) % 4;
                if (region == 0)
                    c = 0x3080c0; // constant
                else if (region == 1)
                    c = static_cast<uint32_t>(x * 4) | static_cast<uint32_t>(z * 3) << 8 | 0x400000; // gradients
                else
                    c = rng() & 0xffffff; // noise: lossy/raw tiles
                m.SetColor(x, y, z, c);
            }
    return ngmap::WriteVxl(m);
}

struct FuzzCount {
    int ok = 0, rejected = 0;
};

static void DecodeOne(const uint8_t* p, size_t n, FuzzCount& fc) {
    uint8_t* out = nullptr;
    size_t outSize = 0;
    int w = 0, h = 0, d = 0;
    int r = ngmap_decode(p, n, &out, &outSize, &w, &h, &d);
    if (r == NGMAP_OK) {
        fc.ok++;
        // whatever comes out must be a well formed VXL of the stated size
        Map m;
        bool parsed = true;
        try {
            ngmap::ParseVxl(out, outSize, w, h, d, m);
        } catch (const ngmap::Error&) {
            parsed = false;
        }
        CHECK(parsed, "accepted stream decoded to a malformed vxl");
    } else {
        fc.rejected++;
        CHECK(out == nullptr, "error path left an output buffer");
        CHECK(r == NGMAP_ERR_CORRUPT || r == NGMAP_ERR_TOO_LARGE, "unexpected error %d (%s)", r, ngmap_strerror(r));
    }
    std::free(out);
}

static void TestMalformed(const fs::path* realMap) {
    const int W = 64, H = 64, D = 64;
    std::vector<uint8_t> vxl = SyntheticVxl(W, H, D, 1234);
    uint8_t* ng = nullptr;
    size_t ngSize = 0;
    int r = ngmap_encode(vxl.data(), vxl.size(), W, H, D, 70, &ng, &ngSize);
    CHECK(r == NGMAP_OK, "synthetic encode: %s", ngmap_strerror(r));
    if (r != NGMAP_OK)
        return;
    std::vector<uint8_t> stream(ng, ng + ngSize);
    std::free(ng);
    std::vector<uint8_t> payload = Inflate(stream);

    {
        FuzzCount fc;
        DecodeOne(stream.data(), stream.size(), fc);
        CHECK(fc.ok == 1, "synthetic map does not decode");
    }

    std::mt19937 rng(42);
    FuzzCount trunc, flips, ptrunc, pflips, junk;

    // truncated compressed stream at every length up to 4 KiB, then a stride
    for (size_t n = 0; n < stream.size(); n += (n < 4096 ? 1 : 97))
        DecodeOne(stream.data(), n, trunc);

    // bit flips in the compressed stream (mostly caught by inflate)
    for (int i = 0; i < 3000; i++) {
        std::vector<uint8_t> s = stream;
        int k = 1 + static_cast<int>(rng() % 4);
        for (int j = 0; j < k; j++)
            s[rng() % s.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
        DecodeOne(s.data(), s.size(), flips);
    }

    // the same on the decompressed payload, re-deflated, so the corruption
    // reaches the ngmap parser instead of stopping in zlib
    for (size_t n = 0; n < payload.size(); n += (n < 2048 ? 1 : 61)) {
        std::vector<uint8_t> p(payload.begin(), payload.begin() + static_cast<long>(n));
        std::vector<uint8_t> s = Deflate(p, 1);
        DecodeOne(s.data(), s.size(), ptrunc);
    }
    for (int i = 0; i < 6000; i++) {
        std::vector<uint8_t> p = payload;
        int k = 1 + static_cast<int>(rng() % 8);
        for (int j = 0; j < k; j++) {
            // bias half of the flips towards the header and the geometry
            size_t at = (rng() & 1) ? rng() % std::min<size_t>(p.size(), 4096) : rng() % p.size();
            p[at] ^= static_cast<uint8_t>(1u << (rng() % 8));
        }
        std::vector<uint8_t> s = Deflate(p, 1);
        DecodeOne(s.data(), s.size(), pflips);
    }

    // hand-made hostile headers
    {
        auto header = [](unsigned w, unsigned h, unsigned d, unsigned q) {
            std::vector<uint8_t> p = {0x8a, 0x14, 0x45, 0x11};
            for (unsigned v : {w, h, d}) {
                p.push_back(static_cast<uint8_t>(v));
                p.push_back(static_cast<uint8_t>(v >> 8));
            }
            p.push_back(static_cast<uint8_t>(q));
            return p;
        };
        struct Case {
            std::vector<uint8_t> p;
            int expect;
        } cases[] = {
            {header(65535, 65535, 64, 80), NGMAP_ERR_TOO_LARGE},
            {header(4096, 512, 64, 80), NGMAP_ERR_TOO_LARGE},
            {header(512, 512, 65, 80), NGMAP_ERR_TOO_LARGE},
            {header(0, 512, 64, 80), NGMAP_ERR_CORRUPT},
            {header(512, 512, 64, 0), NGMAP_ERR_CORRUPT},
            {header(512, 512, 64, 101), NGMAP_ERR_CORRUPT},
            {header(2048, 2048, 64, 80), NGMAP_ERR_CORRUPT}, // valid header, no geometry
        };
        for (auto& c : cases) {
            std::vector<uint8_t> s = Deflate(c.p, 1);
            uint8_t* out = nullptr;
            size_t outSize = 0;
            int w, h, d;
            int got = ngmap_decode(s.data(), s.size(), &out, &outSize, &w, &h, &d);
            CHECK(got == c.expect, "hostile header: got %d expected %d", got, c.expect);
            std::free(out);
            junk.rejected += got != NGMAP_OK;
        }
        // garbage that is not even zlib
        std::vector<uint8_t> g(1000);
        for (auto& b : g)
            b = static_cast<uint8_t>(rng());
        DecodeOne(g.data(), g.size(), junk);
        DecodeOne(g.data(), 0, junk);
    }

    std::printf("malformed input (synthetic %dx%dx%d map, stream %zu bytes, payload %zu bytes):\n", W, H, D,
                stream.size(), payload.size());
    std::printf("  truncated stream     %5d rejected %5d decoded\n", trunc.rejected, trunc.ok);
    std::printf("  stream bit flips     %5d rejected %5d decoded\n", flips.rejected, flips.ok);
    std::printf("  truncated payload    %5d rejected %5d decoded\n", ptrunc.rejected, ptrunc.ok);
    std::printf("  payload bit flips    %5d rejected %5d decoded\n", pflips.rejected, pflips.ok);
    std::printf("  hostile headers/junk %5d rejected %5d decoded\n", junk.rejected, junk.ok);
    CHECK(ptrunc.ok == 0, "a truncated payload decoded");

    // truncations of a real map's stream, sparse because each costs a
    // partial decode of a 512x512 map
    if (realMap) {
        std::vector<uint8_t> rv = ReadFile(*realMap);
        uint8_t* rng_ = nullptr;
        size_t rs = 0;
        if (ngmap_encode(rv.data(), rv.size(), 512, 512, 64, 80, &rng_, &rs) == NGMAP_OK) {
            FuzzCount fc;
            for (int i = 0; i < 40; i++)
                DecodeOne(rng_, rs * static_cast<size_t>(i) / 40, fc);
            std::vector<uint8_t> p = Inflate(std::vector<uint8_t>(rng_, rng_ + rs));
            for (int i = 0; i < 40; i++) {
                std::vector<uint8_t> t(p.begin(), p.begin() + static_cast<long>(p.size() * static_cast<size_t>(i) / 40));
                std::vector<uint8_t> s = Deflate(t, 1);
                DecodeOne(s.data(), s.size(), fc);
            }
            std::printf("  %s truncations   %5d rejected %5d decoded\n", realMap->stem().string().c_str(),
                        fc.rejected, fc.ok);
            CHECK(fc.ok == 0, "a truncated real map decoded");
        }
        std::free(rng_);
    }
}

static void TestApi() {
    uint8_t* out = nullptr;
    size_t n = 0;
    int w, h, d;
    uint8_t one = 0;
    CHECK(ngmap_encode(nullptr, 0, 512, 512, 64, 80, &out, &n) == NGMAP_ERR_ARGS, "null vxl");
    CHECK(ngmap_encode(&one, 1, 512, 512, 64, 0, &out, &n) == NGMAP_ERR_ARGS, "quality 0");
    CHECK(ngmap_encode(&one, 1, 512, 512, 64, 101, &out, &n) == NGMAP_ERR_ARGS, "quality 101");
    CHECK(ngmap_encode(&one, 1, 4096, 512, 64, 80, &out, &n) == NGMAP_ERR_TOO_LARGE, "too wide");
    CHECK(ngmap_encode(&one, 1, 512, 512, 64, 80, &out, &n) == NGMAP_ERR_VXL, "truncated vxl");
    CHECK(ngmap_decode(nullptr, 0, &out, &n, &w, &h, &d) == NGMAP_ERR_ARGS, "null stream");
    CHECK(ngmap_c_api_check() == 0, "C translation unit");
}

int main(int argc, char** argv) {
    fs::path dir = argc > 1 ? argv[1] : NGMAP_DEFAULT_MAPS;
    TestApi();

    std::vector<fs::path> maps;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_directory())
            continue;
        for (auto& f : fs::directory_iterator(e.path(), ec))
            if (f.path().extension() == ".vxl")
                maps.push_back(f.path());
    }
    std::sort(maps.begin(), maps.end());
    if (maps.empty())
        std::printf("no maps under %s, skipping the round trips\n", dir.string().c_str());
    for (auto& m : maps)
        TestMap(m);

    TestMalformed(maps.empty() ? nullptr : &maps.front());

    if (failures) {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all ngmap tests passed\n");
    return 0;
}
