// Internal voxel map used by the ngmap codec (not part of the C API).
//
// Colours are stored only for the voxels that carry one (sparse, per column),
// because a dense colour array for the largest accepted map (2048x2048x64)
// would need 1 GiB.
#ifndef NGMAP_VXL_H
#define NGMAP_VXL_H

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "ngmap.h"

namespace ngmap {

// Carries an NGMAP_ERR_* code to the C boundary, where it is caught.
struct Error : std::runtime_error {
    int code;
    Error(int c, const char* what) : std::runtime_error(what), code(c) {}
};

// Limits shared by the encoder and the decoder: the decoder reads data from
// untrusted servers, so every allocation has to be bounded by them.
enum {
    kMaxWidth = NGMAP_MAX_WIDTH,
    kMaxHeight = NGMAP_MAX_HEIGHT,
    kMaxDepth = NGMAP_MAX_DEPTH, // columns are 64-bit solid masks
};
// A checkerboard 2048x2048x64 map would colour half of 268M voxels (1 GiB of
// colours); real maps have a few million visible voxels, so cap far below.
static const uint64_t kMaxColors = 64ull * 1024 * 1024;

// Colour of a solid voxel without a stored colour (libvxl's default brown),
// in the codec's 0x00BBGGRR layout.
static const uint32_t kDefaultColor = 0x284067u;

struct Map {
    int w = 0, h = 0, d = 0;
    std::vector<uint64_t> solid; // bit z of column (x, y): voxel z is solid
    std::vector<uint64_t> cmask; // bit z: voxel z has a stored colour
    std::vector<uint32_t> coff;  // first colour of each column, w*h+1 entries
    std::vector<uint32_t> colors; // 0x00BBGGRR (R in the low byte, like OpenSpades)

    uint64_t DepthMask() const { return d >= 64 ? ~0ull : ((1ull << d) - 1); }
    size_t Index(int x, int y) const { return static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x); }
    uint64_t Solid(int x, int y) const { return solid[Index(x, y)]; }

    // Colour of voxel z, kDefaultColor when none is stored.
    uint32_t GetColor(int x, int y, int z) const;
    // Only voxels in cmask can be set; others are silently ignored.
    void SetColor(int x, int y, int z, uint32_t c);

    // Sizes cmask's colour storage after cmask is filled (colours start as
    // kDefaultColor). Throws when the colour count exceeds kMaxColors.
    void AllocateColors();
};

void CheckDimensions(int w, int h, int d);

// Parses AoS VXL bytes. Throws Error on malformed input.
void ParseVxl(const uint8_t* data, size_t size, int w, int h, int d, Map& out);

// Serialises the map as AoS VXL. The top and bottom of every solid run get a
// colour (kDefaultColor when none is stored), as VXL requires. A column that
// ends in air is written as a last span with S = depth and no colours, which
// is how real maps (Empty_ocean) store all-air columns.
std::vector<uint8_t> WriteVxl(const Map& map);

} // namespace ngmap

#endif
