#include "block3d/converter.hpp"
#include "block3d/reader.hpp"
#include <cstring>
#include <fstream>
#include <vector>
#include <algorithm>
#include <iostream>
#include <thread>
#include <atomic>
#include <future>
#include <stdexcept>

#ifdef OMP_ENABLED
#include <omp.h>
#endif

namespace block3d {

struct BlockData {
    uint64_t linear_idx;
    std::vector<float> data;
};

static void validate_convert_options(uint64_t block_size,
                                     BlockInnerLayout inner_layout,
                                     uint32_t micro_size) {
    if (inner_layout == BlockInnerLayout::LegacyXYZ) {
        if (micro_size != 0) {
            throw std::invalid_argument("legacy layout requires micro_size=0");
        }
        return;
    }

    if (inner_layout != BlockInnerLayout::MicroTiledXYZ) {
        throw std::invalid_argument("unknown block inner layout");
    }
    if (micro_size != DEFAULT_MICRO_SIZE) {
        throw std::invalid_argument("micro-tiled layout requires micro_size=8");
    }
    if (block_size % micro_size != 0) {
        throw std::invalid_argument("block_size must be divisible by micro_size");
    }
}

static void extract_block(uint32_t bx, uint32_t by_, uint32_t bz,
                          const float* raw_3d,
                          uint64_t dim_x, uint64_t dim_y, uint64_t dim_z,
                          uint64_t bs,
                          BlockInnerLayout inner_layout,
                          uint32_t micro_size,
                          float* out) {
    uint64_t x0 = static_cast<uint64_t>(bx) * bs;
    uint64_t y0 = static_cast<uint64_t>(by_) * bs;
    uint64_t z0 = static_cast<uint64_t>(bz) * bs;
    uint64_t nx = std::min(bs, dim_x - x0);
    uint64_t ny = std::min(bs, dim_y - y0);
    uint64_t nz = std::min(bs, dim_z - z0);

    if (inner_layout == BlockInnerLayout::LegacyXYZ) {
        if (nx == bs && ny == bs && nz == bs) {
            for (uint64_t lx = 0; lx < bs; lx++) {
                uint64_t raw_base = (x0 + lx) * dim_y * dim_z + y0 * dim_z + z0;
                for (uint64_t ly = 0; ly < bs; ly++) {
                    std::memcpy(&out[legacy_local_offset(bs,
                                                         static_cast<uint32_t>(lx),
                                                         static_cast<uint32_t>(ly),
                                                         0)],
                                &raw_3d[raw_base + ly * dim_z],
                                bs * sizeof(float));
                }
            }
        } else {
            std::memset(out, 0, bs * bs * bs * sizeof(float));
            for (uint64_t lx = 0; lx < nx; lx++) {
                uint64_t raw_base = (x0 + lx) * dim_y * dim_z + y0 * dim_z + z0;
                for (uint64_t ly = 0; ly < ny; ly++) {
                    std::memcpy(&out[legacy_local_offset(bs,
                                                         static_cast<uint32_t>(lx),
                                                         static_cast<uint32_t>(ly),
                                                         0)],
                                &raw_3d[raw_base + ly * dim_z],
                                nz * sizeof(float));
                }
            }
        }
        return;
    }

    std::memset(out, 0, bs * bs * bs * sizeof(float));
    for (uint32_t lx = 0; lx < nx; lx++) {
        uint64_t raw_base = (x0 + lx) * dim_y * dim_z + y0 * dim_z + z0;
        for (uint32_t ly = 0; ly < ny; ly++) {
            for (uint32_t lz = 0; lz < nz; lz++) {
                uint64_t dst = micro_tiled_local_offset(bs, micro_size, lx, ly, lz);
                out[dst] = raw_3d[raw_base + static_cast<uint64_t>(ly) * dim_z + lz];
            }
        }
    }
}

void convert_raw_to_blocked(
    const std::string& raw_path,
    const std::string& output_path,
    uint64_t dim_x, uint64_t dim_y, uint64_t dim_z,
    const ConvertOptions& options)
{
    uint64_t block_size = options.block_size;
    int num_threads = options.num_threads;
    bool progress = options.progress;
    uint64_t max_memory_mb = options.max_memory_mb;
    BlockInnerLayout inner_layout = options.inner_layout;
    uint32_t micro_size = options.micro_size;
    validate_convert_options(block_size, inner_layout, micro_size);

    if (num_threads <= 0)
        num_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (num_threads < 1) num_threads = 1;

    BlockLayout3D layout(dim_x, dim_y, dim_z, block_size);
    uint64_t bs = block_size;

    MappedFile raw_file(raw_path);
    const float* raw_3d = raw_file.data();

    auto ordered_blocks = layout.block_order();

    std::ofstream out(output_path, std::ios::binary);
    if (!out)
        throw std::runtime_error("Cannot create output file: " + output_path);

    FileHeader header;
    std::memcpy(header.magic, MAGIC, 4);
    header.version      = inner_layout == BlockInnerLayout::MicroTiledXYZ
                         ? VERSION_MICROTILE
                         : VERSION_LEGACY;
    header.dim_x        = dim_x;
    header.dim_y        = dim_y;
    header.dim_z        = dim_z;
    header.block_size   = block_size;
    header.total_blocks = layout.total_blocks;
    header.data_offset  = aligned_data_offset(layout.total_blocks);
    header.reserved     = inner_layout == BlockInnerLayout::MicroTiledXYZ
                         ? encode_layout(static_cast<uint8_t>(LAYOUT_MICRO_TILED_XYZ),
                                         static_cast<uint8_t>(micro_size))
                         : 0;

    out.write(reinterpret_cast<const char*>(&header), sizeof(header));

    uint64_t offset_placeholder = 0;
    for (uint64_t i = 0; i < layout.total_blocks; i++)
        out.write(reinterpret_cast<const char*>(&offset_placeholder),
                  sizeof(uint64_t));

    uint64_t current_pos = static_cast<uint64_t>(out.tellp());
    if (current_pos < header.data_offset) {
        std::vector<char> pad(header.data_offset - current_pos, 0);
        out.write(pad.data(), pad.size());
    }

    size_t total = ordered_blocks.size();

    size_t batch_size;
    if (max_memory_mb > 0) {
        uint64_t bytes_per_block = layout.block_bytes;
        uint64_t max_bytes = max_memory_mb * 1024 * 1024;
        uint64_t max_blocks_in_mem = max_bytes / bytes_per_block;
        if (max_blocks_in_mem < 1) max_blocks_in_mem = 1;
        batch_size = std::max<size_t>(1,
            std::min<size_t>(total, static_cast<size_t>(max_blocks_in_mem)));
    } else {
        batch_size = total;
    }

    if (num_threads > 1 && batch_size > static_cast<size_t>(num_threads) * 4) {
        size_t min_per_thread = 16;
        batch_size = std::max(static_cast<size_t>(num_threads) * min_per_thread,
                              batch_size / 4);
    }
    if (batch_size > total) batch_size = total;
    if (batch_size < 1) batch_size = 1;

    std::vector<uint64_t> block_offsets(layout.total_blocks);
    std::atomic<size_t> done{0};

    auto print_progress = [&](const char* prefix) {
        if (!progress) return;
        size_t d = done.load();
        if (d % 100 == 0 || d == total) {
            std::cout << "\r" << prefix << " " << d << "/" << total
                      << " blocks" << std::flush;
        }
    };

    // -- Double-buffered pipeline ----------------------------------------
    // Two block pools so extraction of batch N+1 can overlap with the
    // (sequential, I/O-bound) write of batch N.  The first batch is
    // extracted synchronously; every subsequent batch is extracted via
    // std::async while the main thread writes the previous batch.
    std::vector<float> block_pool[2];
    block_pool[0].resize(batch_size * layout.block_floats);
    block_pool[1].resize(batch_size * layout.block_floats);

    std::vector<BlockData> batch_data[2];

    auto extract_batch = [&](size_t start, size_t len,
                              std::vector<float>& pool,
                              std::vector<BlockData>& meta) {
        meta.resize(len);
#ifdef OMP_ENABLED
        #pragma omp parallel for num_threads(num_threads)
#endif
        for (int64_t i = 0; i < static_cast<int64_t>(len); i++) {
            auto [bx, by_, bz] = ordered_blocks[start + i];
            meta[i].linear_idx = layout.linear_index(bx, by_, bz);
            float* dst = pool.data() + i * layout.block_floats;
            extract_block(bx, by_, bz, raw_3d,
                          dim_x, dim_y, dim_z, bs,
                          inner_layout, micro_size, dst);
        }
    };

    auto write_batch = [&](const std::vector<BlockData>& meta,
                            const std::vector<float>& pool,
                            size_t len) {
        for (size_t i = 0; i < len; i++) {
            uint64_t off = static_cast<uint64_t>(out.tellp());
            block_offsets[meta[i].linear_idx] = off;
            const float* src = pool.data() + i * layout.block_floats;
            out.write(
                reinterpret_cast<const char*>(src),
                static_cast<std::streamsize>(layout.block_bytes));
            done++;
            print_progress("Converting");
        }
    };

    // Phase 0: extract first batch (nothing to overlap with).
    size_t first_len = std::min(batch_size, total);
    extract_batch(0, first_len, block_pool[0], batch_data[0]);

    // Phase 1..N-1: pipeline -- extract batch N while writing batch N-1.
    // bi is the 1-based batch index within the pipeline loop.
    for (size_t bs = batch_size, bi = 1; bs < total; bs += batch_size, bi++) {
        size_t len = std::min(batch_size, total - bs);
        int write_idx   = static_cast<int>((bi - 1) % 2);
        int extract_idx = static_cast<int>(bi % 2);

        auto fut = std::async(std::launch::async, [&]() {
            extract_batch(bs, len,
                          block_pool[extract_idx], batch_data[extract_idx]);
        });

        // Write the *previous* batch (I/O on main thread) concurrently
        // with the async extraction above.
        size_t prev_len = std::min(batch_size, total - (bs - batch_size));
        write_batch(batch_data[write_idx], block_pool[write_idx], prev_len);

        fut.get();
    }

    // Phase final: write the last batch.
    {
        size_t num_batches = (total + batch_size - 1) / batch_size;
        size_t last_start  = (num_batches > 0) ? (num_batches - 1) * batch_size : 0;
        size_t last_len    = total - last_start;
        int    last_pool   = (num_batches > 0)
                             ? static_cast<int>((num_batches - 1) % 2) : 0;
        write_batch(batch_data[last_pool], block_pool[last_pool], last_len);
    }

    if (progress) std::cout << "\n";

    out.seekp(HEADER_SIZE);
    out.write(reinterpret_cast<const char*>(block_offsets.data()),
              static_cast<std::streamsize>(layout.total_blocks * sizeof(uint64_t)));

    out.seekp(0);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));

    out.close();

    if (progress) {
        std::cout << "Conversion complete: " << output_path << std::endl;
    }
}

void convert_raw_to_blocked(
    const std::string& raw_path,
    const std::string& output_path,
    uint64_t dim_x, uint64_t dim_y, uint64_t dim_z,
    uint64_t block_size,
    int num_threads,
    bool progress,
    uint64_t max_memory_mb)
{
    ConvertOptions options;
    options.block_size = block_size;
    options.num_threads = num_threads;
    options.progress = progress;
    options.max_memory_mb = max_memory_mb;
    convert_raw_to_blocked(raw_path, output_path, dim_x, dim_y, dim_z, options);
}

} // namespace block3d
