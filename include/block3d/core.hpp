#pragma once
#include <cstdint>
#include <tuple>
#include <vector>
#include <string>
#include "types.hpp"

namespace block3d {

// ── Storage medium classification ─────────────────────────────────────

enum class StorageClass { HDD, SSD, NVMe, Unknown };

/// Quick write+fsync test to classify the storage underlying `output_dir`.
/// Creates and removes a small temp file; returns Unknown on failure.
StorageClass detect_storage_medium(const std::string& output_dir);

/// Heuristically choose a block_size that balances X/Y/Z slice performance
/// for the given volume dimensions and storage medium.
/// Returns a value in [16, 256] that is a multiple of 4.
uint64_t auto_block_size(uint64_t dim_x, uint64_t dim_y, uint64_t dim_z,
                         StorageClass medium);

inline uint64_t spread_bits(uint32_t v) {
    uint64_t x = v & 0x1FFFFFULL;
    x = (x | (x << 32)) & 0x001F00000000FFFFULL;
    x = (x | (x << 16)) & 0x001F0000FF0000FFULL;
    x = (x | (x << 8))  & 0x100F00F00F00F00FULL;
    x = (x | (x << 4))  & 0x10C30C30C30C30C3ULL;
    x = (x | (x << 2))  & 0x1249249249249249ULL;
    return x;
}

inline uint32_t compact_bits(uint64_t v) {
    v &= 0x1249249249249249ULL;
    v = (v ^ (v >> 2))  & 0x10C30C30C30C30C3ULL;
    v = (v ^ (v >> 4))  & 0x100F00F00F00F00FULL;
    v = (v ^ (v >> 8))  & 0x001F0000FF0000FFULL;
    v = (v ^ (v >> 16)) & 0x001F00000000FFFFULL;
    v = (v ^ (v >> 32)) & 0x1FFFFFULL;
    return static_cast<uint32_t>(v);
}

inline uint64_t morton_encode(uint32_t bx, uint32_t by_, uint32_t bz) {
    return spread_bits(bx) | (spread_bits(by_) << 1) | (spread_bits(bz) << 2);
}

inline std::tuple<uint32_t, uint32_t, uint32_t>
morton_decode(uint64_t code) {
    uint32_t bx = compact_bits(code);
    uint32_t by_ = compact_bits(code >> 1);
    uint32_t bz = compact_bits(code >> 2);
    return {bx, by_, bz};
}

struct BlockLayout3D {
    uint64_t dim_x, dim_y, dim_z;
    uint64_t block_size;
    uint64_t blocks_x, blocks_y, blocks_z;
    uint64_t total_blocks;
    uint64_t block_floats;
    uint64_t block_bytes;

    BlockLayout3D() = default;

    BlockLayout3D(uint64_t dx, uint64_t dy, uint64_t dz, uint64_t bs)
        : dim_x(dx), dim_y(dy), dim_z(dz), block_size(bs)
    {
        blocks_x = (dx + bs - 1) / bs;
        blocks_y = (dy + bs - 1) / bs;
        blocks_z = (dz + bs - 1) / bs;
        total_blocks = blocks_x * blocks_y * blocks_z;
        block_floats = bs * bs * bs;
        block_bytes = block_floats * 4;
    }

    uint64_t linear_index(uint32_t bx, uint32_t by_, uint32_t bz) const {
        return static_cast<uint64_t>(bx) * blocks_y * blocks_z
             + static_cast<uint64_t>(by_) * blocks_z
             + bz;
    }

    uint64_t block_offset(uint32_t bx, uint32_t by_, uint32_t bz) const {
        return linear_index(bx, by_, bz) * block_bytes;
    }

    std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>
    global_to_block(uint64_t x, uint64_t y, uint64_t z) const {
        uint32_t bx = static_cast<uint32_t>(x / block_size);
        uint32_t by_ = static_cast<uint32_t>(y / block_size);
        uint32_t bz = static_cast<uint32_t>(z / block_size);
        uint32_t lx = static_cast<uint32_t>(x % block_size);
        uint32_t ly = static_cast<uint32_t>(y % block_size);
        uint32_t lz = static_cast<uint32_t>(z % block_size);
        return {bx, by_, bz, lx, ly, lz};
    }

    std::tuple<uint64_t, uint64_t, uint64_t>
    block_origin(uint32_t bx, uint32_t by_, uint32_t bz) const {
        return {bx * block_size, by_ * block_size, bz * block_size};
    }

    std::tuple<uint64_t, uint64_t, uint64_t>
    block_extent(uint32_t bx, uint32_t by_, uint32_t bz) const {
        auto [x0, y0, z0] = block_origin(bx, by_, bz);
        uint64_t nx = std::min(block_size, dim_x - x0);
        uint64_t ny = std::min(block_size, dim_y - y0);
        uint64_t nz = std::min(block_size, dim_z - z0);
        return {nx, ny, nz};
    }

    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> block_order() const;
};

} // namespace block3d
