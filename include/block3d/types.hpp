#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace block3d {

#pragma pack(push, 1)
struct FileHeader {
    char     magic[4];       // "3DBK"
    uint32_t version;        // 1
    uint64_t dim_x;
    uint64_t dim_y;
    uint64_t dim_z;
    uint64_t block_size;
    uint64_t total_blocks;
    uint64_t data_offset;    // byte offset of first block data
    uint64_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(FileHeader) == 4 + 4 + 7 * 8,
              "FileHeader must be 64 bytes");

constexpr const char* MAGIC = "3DBK";
constexpr uint32_t VERSION_LEGACY = 1;
constexpr uint32_t VERSION_MICROTILE = 2;
constexpr uint32_t VERSION = VERSION_LEGACY;
constexpr uint64_t LAYOUT_LEGACY_XYZ = 0;
constexpr uint64_t LAYOUT_MICRO_TILED_XYZ = 1;
constexpr uint32_t DEFAULT_MICRO_SIZE = 8;
constexpr uint64_t HEADER_SIZE = sizeof(FileHeader);
constexpr uint64_t PAGE_ALIGN = 4096;

enum class BlockInnerLayout {
    LegacyXYZ,
    MicroTiledXYZ,
};

inline uint64_t encode_layout(uint8_t layout_kind, uint8_t micro_size) {
    return static_cast<uint64_t>(layout_kind)
         | (static_cast<uint64_t>(micro_size) << 8);
}

inline uint8_t header_layout_kind(uint64_t reserved) {
    return static_cast<uint8_t>(reserved & 0xffu);
}

inline uint8_t header_micro_size(uint64_t reserved) {
    return static_cast<uint8_t>((reserved >> 8) & 0xffu);
}

inline uint64_t legacy_local_offset(uint64_t bs,
                                    uint32_t lx, uint32_t ly, uint32_t lz) {
    return static_cast<uint64_t>(lx) * bs * bs
         + static_cast<uint64_t>(ly) * bs
         + lz;
}

inline uint64_t micro_tiled_local_offset(uint64_t bs, uint32_t micro_size,
                                         uint32_t lx, uint32_t ly, uint32_t lz) {
    const uint64_t m = micro_size;
    const uint64_t micro_count = bs / m;
    const uint64_t mx = lx / m;
    const uint64_t my = ly / m;
    const uint64_t mz = lz / m;
    const uint64_t ix = lx % m;
    const uint64_t iy = ly % m;
    const uint64_t iz = lz % m;
    const uint64_t micro_linear = mx * micro_count * micro_count
                                + my * micro_count
                                + mz;
    const uint64_t inner_linear = ix * m * m + iy * m + iz;
    return micro_linear * m * m * m + inner_linear;
}

inline uint64_t local_offset_for_layout(BlockInnerLayout layout,
                                        uint64_t block_size,
                                        uint32_t micro_size,
                                        uint32_t lx, uint32_t ly, uint32_t lz) {
    return layout == BlockInnerLayout::MicroTiledXYZ
        ? micro_tiled_local_offset(block_size, micro_size, lx, ly, lz)
        : legacy_local_offset(block_size, lx, ly, lz);
}

inline uint64_t aligned_data_offset(uint64_t total_blocks) {
    uint64_t off = HEADER_SIZE + total_blocks * sizeof(uint64_t);
    return (off + PAGE_ALIGN - 1) & ~(PAGE_ALIGN - 1);
}

inline std::string str_axis(char axis) {
    return std::string(1, axis);
}

} // namespace block3d
