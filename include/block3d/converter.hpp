#pragma once
#include <string>
#include <cstdint>
#include "core.hpp"

namespace block3d {

struct ConvertOptions {
    uint64_t block_size = 32;
    int num_threads = 0;
    bool progress = true;
    uint64_t max_memory_mb = 0;
    BlockInnerLayout inner_layout = BlockInnerLayout::LegacyXYZ;
    uint32_t micro_size = 0;
};

void convert_raw_to_blocked(
    const std::string& raw_path,
    const std::string& output_path,
    uint64_t dim_x, uint64_t dim_y, uint64_t dim_z,
    const ConvertOptions& options);

void convert_raw_to_blocked(
    const std::string& raw_path,
    const std::string& output_path,
    uint64_t dim_x, uint64_t dim_y, uint64_t dim_z,
    uint64_t block_size = 32,
    int num_threads = 0,
    bool progress = true,
    uint64_t max_memory_mb = 0);

} // namespace block3d
