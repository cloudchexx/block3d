#include "block3d/core.hpp"
#include "block3d/reader.hpp"
#include "block3d/converter.hpp"
#include "block3d/rng.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>
#include <random>
#include <thread>
#include <string>
#include <atomic>

namespace fs = std::filesystem;

using namespace block3d;

static bool allclose(const std::vector<float>& a,
                     const std::vector<float>& b,
                     float rtol = 1e-5f, float atol = 1e-6f) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        float diff = std::abs(a[i] - b[i]);
        float max_abs = std::max(std::abs(a[i]), std::abs(b[i]));
        if (diff > atol && diff > rtol * max_abs) return false;
    }
    return true;
}

static int test_format_layout_helpers() {
    std::cout << "  Format layout helpers...";

    if (VERSION != VERSION_LEGACY || VERSION_LEGACY != 1 || VERSION_MICROTILE != 2) {
        std::cerr << "FAIL version constants\n";
        return 1;
    }
    uint64_t encoded = encode_layout(static_cast<uint8_t>(LAYOUT_MICRO_TILED_XYZ),
                                     static_cast<uint8_t>(DEFAULT_MICRO_SIZE));
    if (header_layout_kind(encoded) != LAYOUT_MICRO_TILED_XYZ ||
        header_micro_size(encoded) != DEFAULT_MICRO_SIZE) {
        std::cerr << "FAIL layout encode/decode\n";
        return 1;
    }
    if (legacy_local_offset(16, 1, 2, 3) != 1 * 16 * 16 + 2 * 16 + 3) {
        std::cerr << "FAIL legacy local offset\n";
        return 1;
    }
    if (micro_tiled_local_offset(16, 8, 0, 8, 0) != 1024) {
        std::cerr << "FAIL micro local offset\n";
        return 1;
    }
    if (micro_tiled_local_offset(16, 8, 0, 0, 8) != 512) {
        std::cerr << "FAIL micro z-tile offset\n";
        return 1;
    }
    if (local_offset_for_layout(BlockInnerLayout::LegacyXYZ, 16, 0, 0, 8, 0) ==
        local_offset_for_layout(BlockInnerLayout::MicroTiledXYZ, 16, 8, 0, 8, 0)) {
        std::cerr << "FAIL layout offsets should differ\n";
        return 1;
    }

    std::cout << "PASS\n";
    return 0;
}

static int test_morton() {
    std::cout << "  Morton encode/decode roundtrip...";
    for (uint32_t bx = 0; bx < 64; bx++) {
        for (uint32_t by_ = 0; by_ < 64; by_++) {
            for (uint32_t bz = 0; bz < 64; bz++) {
                uint64_t code = morton_encode(bx, by_, bz);
                auto [rbx, rby, rbz] = morton_decode(code);
                if (bx != rbx || by_ != rby || bz != rbz) {
                    std::cerr << "FAIL at (" << bx << "," << by_ << "," << bz
                              << ") -> (" << rbx << "," << rby << "," << rbz << ")\n";
                    return 1;
                }
            }
        }
    }

    const std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> high_cases = {
        {255, 255, 255}, {256, 0, 0}, {0, 256, 0}, {0, 0, 256},
        {511, 513, 1025}, {4096, 8191, 16383},
        {0x1FFFFF, 0x1FFFFF, 0x1FFFFF}
    };
    for (const auto& [bx, by_, bz] : high_cases) {
        uint64_t code = morton_encode(bx, by_, bz);
        auto [rbx, rby, rbz] = morton_decode(code);
        if (bx != rbx || by_ != rby || bz != rbz) {
            std::cerr << "FAIL high case at (" << bx << "," << by_ << "," << bz
                      << ") -> (" << rbx << "," << rby << "," << rbz << ")\n";
            return 1;
        }
    }

    if (morton_encode(256, 0, 0) == morton_encode(0, 0, 0)) {
        std::cerr << "FAIL: bit 8 was not preserved\n";
        return 1;
    }

    std::cout << "PASS\n";
    return 0;
}

static int test_block_layout() {
    std::cout << "  BlockLayout3D basic...";
    BlockLayout3D layout(100, 200, 300, 32);
    if (layout.blocks_x != 4 || layout.blocks_y != 7 || layout.blocks_z != 10) {
        std::cerr << "FAIL: unexpected block counts\n";
        return 1;
    }
    if (layout.total_blocks != 280) {
        std::cerr << "FAIL: total_blocks=" << layout.total_blocks << "\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

static int test_block_order() {
    std::cout << "  BlockLayout3D block_order...";
    BlockLayout3D layout(100, 200, 300, 32);
    auto order = layout.block_order();
    if (order.size() != layout.total_blocks) {
        std::cerr << "FAIL: size mismatch " << order.size()
                  << " vs " << layout.total_blocks << "\n";
        return 1;
    }
    bool sorted = true;
    for (size_t i = 1; i < order.size(); i++) {
        auto [ax, ay, az] = order[i - 1];
        auto [bx, by_, bz] = order[i];
        if (morton_encode(ax, ay, az) >= morton_encode(bx, by_, bz)) {
            sorted = false;
            break;
        }
    }
    if (!sorted) {
        std::cerr << "FAIL: not sorted by Morton order\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

static int test_roundtrip() {
    std::cout << "  Full roundtrip (convert + read)...";

    uint64_t dx = 16, dy = 20, dz = 24;
    uint64_t total = dx * dy * dz;
    std::vector<float> raw_data(total);

    XorShift32 rng(42);
    for (size_t i = 0; i < total; i++)
        raw_data[i] = rng.next_float();

    fs::path tmp = fs::temp_directory_path() / "block3d_test";
    fs::create_directories(tmp);
    fs::path raw_path = tmp / "test_raw.dat";
    fs::path b3d_path = tmp / "test.b3d";

    {
        std::ofstream f(raw_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(raw_data.data()),
                raw_data.size() * sizeof(float));
    }

    convert_raw_to_blocked(raw_path.string(), b3d_path.string(),
                           dx, dy, dz, 8, 2, false);

    BlockedFileReader reader(b3d_path.string());
    auto result = reader.read_full_volume();

    for (size_t i = 0; i < total; i++) {
        float diff = std::abs(result[i] - raw_data[i]);
        float max_abs = std::max(std::abs(result[i]), std::abs(raw_data[i]));
        if (diff > 1e-6f && diff > 1e-5f * max_abs) {
            std::cerr << "FAIL at index " << i << ": " << result[i]
                      << " vs " << raw_data[i] << "\n";
            return 1;
        }
    }
    std::cout << "PASS\n";
    return 0;
}

static int test_slice_reads() {
    std::cout << "  Slice reads...";

    uint64_t dx = 16, dy = 20, dz = 24;
    uint64_t total = dx * dy * dz;
    std::vector<float> raw_data(total);
    XorShift32 rng(42);
    for (size_t i = 0; i < total; i++)
        raw_data[i] = rng.next_float();

    auto at = [&](uint64_t x, uint64_t y, uint64_t z) -> float {
        return raw_data[x * dy * dz + y * dz + z];
    };

    fs::path tmp = fs::temp_directory_path() / "block3d_test";
    fs::path raw_path = tmp / "test_slice_raw.dat";
    fs::path b3d_path = tmp / "test_slice.b3d";

    {
        std::ofstream f(raw_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(raw_data.data()),
                raw_data.size() * sizeof(float));
    }

    convert_raw_to_blocked(raw_path.string(), b3d_path.string(),
                           dx, dy, dz, 8, 2, false);

    BlockedFileReader reader(b3d_path.string());

    {
        auto slice = reader.read_x_slice(dx / 2);
        if (slice.size() != dy * dz) {
            std::cerr << "FAIL X-slice size\n"; return 1;
        }
        for (uint64_t y = 0; y < dy; y++)
            for (uint64_t z = 0; z < dz; z++)
                if (std::abs(slice[y * dz + z] - at(dx / 2, y, z)) > 1e-6f) {
                    std::cerr << "FAIL X-slice at (" << y << "," << z << ")\n";
                    return 1;
                }
    }

    {
        auto slice = reader.read_y_slice(dy / 2);
        if (slice.size() != dx * dz) {
            std::cerr << "FAIL Y-slice size\n"; return 1;
        }
        for (uint64_t x = 0; x < dx; x++)
            for (uint64_t z = 0; z < dz; z++)
                if (std::abs(slice[x * dz + z] - at(x, dy / 2, z)) > 1e-6f) {
                    std::cerr << "FAIL Y-slice at (" << x << "," << z << ")\n";
                    return 1;
                }
    }

    {
        auto slice = reader.read_z_slice(dz / 2);
        if (slice.size() != dx * dy) {
            std::cerr << "FAIL Z-slice size\n"; return 1;
        }
        for (uint64_t x = 0; x < dx; x++)
            for (uint64_t y = 0; y < dy; y++)
                if (std::abs(slice[x * dy + y] - at(x, y, dz / 2)) > 1e-6f) {
                    std::cerr << "FAIL Z-slice at (" << x << "," << y << ")\n";
                    return 1;
                }
    }

    std::cout << "PASS\n";
    return 0;
}

static int test_column_reads() {
    std::cout << "  Column reads...";

    uint64_t dx = 16, dy = 20, dz = 24;
    uint64_t total = dx * dy * dz;
    std::vector<float> raw_data(total);
    XorShift32 rng(42);
    for (size_t i = 0; i < total; i++)
        raw_data[i] = rng.next_float();

    auto at = [&](uint64_t x, uint64_t y, uint64_t z) -> float {
        return raw_data[x * dy * dz + y * dz + z];
    };

    fs::path tmp = fs::temp_directory_path() / "block3d_test";
    fs::path raw_path = tmp / "test_col_raw.dat";
    fs::path b3d_path = tmp / "test_col.b3d";

    {
        std::ofstream f(raw_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(raw_data.data()),
                raw_data.size() * sizeof(float));
    }

    convert_raw_to_blocked(raw_path.string(), b3d_path.string(),
                           dx, dy, dz, 8, 2, false);

    BlockedFileReader reader(b3d_path.string());

    {
        auto col = reader.read_x_column(dy / 3, dz / 3);
        if (col.size() != dx) { std::cerr << "FAIL X-col size\n"; return 1; }
        for (uint64_t x = 0; x < dx; x++)
            if (std::abs(col[x] - at(x, dy / 3, dz / 3)) > 1e-6f) {
                std::cerr << "FAIL X-col at " << x << "\n"; return 1;
            }
    }

    {
        auto col = reader.read_y_column(dx / 3, dz / 3);
        if (col.size() != dy) { std::cerr << "FAIL Y-col size\n"; return 1; }
        for (uint64_t y = 0; y < dy; y++)
            if (std::abs(col[y] - at(dx / 3, y, dz / 3)) > 1e-6f) {
                std::cerr << "FAIL Y-col at " << y << "\n"; return 1;
            }
    }

    {
        auto col = reader.read_z_column(dx / 3, dy / 3);
        if (col.size() != dz) { std::cerr << "FAIL Z-col size\n"; return 1; }
        for (uint64_t z = 0; z < dz; z++)
            if (std::abs(col[z] - at(dx / 3, dy / 3, z)) > 1e-6f) {
                std::cerr << "FAIL Z-col at " << z << "\n"; return 1;
            }
    }

    std::cout << "PASS\n";
    return 0;
}

static int test_subvolume() {
    std::cout << "  Subvolume read...";

    uint64_t dx = 16, dy = 20, dz = 24;
    uint64_t total = dx * dy * dz;
    std::vector<float> raw_data(total);
    XorShift32 rng(42);
    for (size_t i = 0; i < total; i++)
        raw_data[i] = rng.next_float();

    auto at = [&](uint64_t x, uint64_t y, uint64_t z) -> float {
        return raw_data[x * dy * dz + y * dz + z];
    };

    fs::path tmp = fs::temp_directory_path() / "block3d_test";
    fs::path raw_path = tmp / "test_subv_raw.dat";
    fs::path b3d_path = tmp / "test_subv.b3d";

    {
        std::ofstream f(raw_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(raw_data.data()),
                raw_data.size() * sizeof(float));
    }

    convert_raw_to_blocked(raw_path.string(), b3d_path.string(),
                           dx, dy, dz, 8, 2, false);

    BlockedFileReader reader(b3d_path.string());

    auto sub = reader.read_subvolume(2, 14, 3, 17, 5, 20);

    uint64_t nx = 12, ny = 14, nz = 15;
    if (sub.size() != nx * ny * nz) {
        std::cerr << "FAIL subvolume size\n"; return 1;
    }

    for (uint64_t x = 2; x < 14; x++)
        for (uint64_t y = 3; y < 17; y++)
            for (uint64_t z = 5; z < 20; z++) {
                uint64_t idx = (x-2) * ny * nz + (y-3) * nz + (z-5);
                if (std::abs(sub[idx] - at(x, y, z)) > 1e-6f) {
                    std::cerr << "FAIL subvolume at (" << x << "," << y
                              << "," << z << ")\n";
                    return 1;
                }
            }

    std::cout << "PASS\n";
    return 0;
}

static int test_storage_ratio() {
    std::cout << "  Storage ratio < 1.5x...";

    uint64_t dx = 16, dy = 20, dz = 24;
    uint64_t total = dx * dy * dz;
    std::vector<float> raw_data(total, 1.0f);

    fs::path tmp = fs::temp_directory_path() / "block3d_test";
    fs::path raw_path = tmp / "test_ratio_raw.dat";
    fs::path b3d_path = tmp / "test_ratio.b3d";

    {
        std::ofstream f(raw_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(raw_data.data()),
                raw_data.size() * sizeof(float));
    }

    convert_raw_to_blocked(raw_path.string(), b3d_path.string(),
                           dx, dy, dz, 8, 2, false);

    double raw_size = static_cast<double>(fs::file_size(raw_path));
    double b3d_size = static_cast<double>(fs::file_size(b3d_path));
    double ratio = b3d_size / raw_size;

    if (ratio >= 1.5) {
        std::cerr << "FAIL: ratio=" << ratio << "\n"; return 1;
    }
    std::cout << "PASS (ratio=" << ratio << ")\n";
    return 0;
}

static int test_verify() {
    std::cout << "  Verify against raw...";

    uint64_t dx = 16, dy = 20, dz = 24;
    uint64_t total = dx * dy * dz;
    std::vector<float> raw_data(total);
    XorShift32 rng(42);
    for (size_t i = 0; i < total; i++)
        raw_data[i] = rng.next_float();

    fs::path tmp = fs::temp_directory_path() / "block3d_test";
    fs::path raw_path = tmp / "test_verify_raw.dat";
    fs::path b3d_path = tmp / "test_verify.b3d";

    {
        std::ofstream f(raw_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(raw_data.data()),
                raw_data.size() * sizeof(float));
    }

    convert_raw_to_blocked(raw_path.string(), b3d_path.string(),
                           dx, dy, dz, 8, 2, false);

    BlockedFileReader reader(b3d_path.string());
    if (!reader.verify(raw_path.string(), 500)) {
        std::cerr << "FAIL: verification returned false\n"; return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

static int test_batch_read() {
    std::cout << "  Batch slice read (element correctness)...";

    uint64_t dx = 19, dy = 21, dz = 23;
    uint64_t bs = 8;
    uint64_t total = dx * dy * dz;
    std::vector<float> raw_data(total);
    for (uint64_t x = 0; x < dx; x++)
        for (uint64_t y = 0; y < dy; y++)
            for (uint64_t z = 0; z < dz; z++)
                raw_data[x * dy * dz + y * dz + z] =
                    static_cast<float>(x * 1000000 + y * 1000 + z);

    auto at = [&](uint64_t x, uint64_t y, uint64_t z) -> float {
        return raw_data[x * dy * dz + y * dz + z];
    };

    fs::path tmp = fs::temp_directory_path() / "block3d_test";
    fs::path raw_path = tmp / "test_batch_raw.dat";
    fs::path b3d_path = tmp / "test_batch.b3d";

    {
        std::ofstream f(raw_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(raw_data.data()),
                raw_data.size() * sizeof(float));
    }

    convert_raw_to_blocked(raw_path.string(), b3d_path.string(),
                           dx, dy, dz, bs, 4, false);

    BlockedFileReader reader(b3d_path.string());

    auto check_batch = [&](char axis,
                           const std::vector<uint64_t>& indices,
                           const char* label) -> bool {
        auto slices = reader.read_slices_batch(axis, indices, 4);
        if (slices.size() != indices.size()) {
            std::cerr << "FAIL batch count axis=" << axis << " case=" << label << "\n";
            return false;
        }
        for (size_t i = 0; i < indices.size(); i++) {
            uint64_t idx = indices[i];
            const auto& sl = slices[i];
            if (axis == 'x') {
                if (sl.size() != dy * dz) {
                    std::cerr << "FAIL x size case=" << label << "\n"; return false;
                }
                for (uint64_t y = 0; y < dy; y++)
                    for (uint64_t z = 0; z < dz; z++)
                        if (std::abs(sl[y * dz + z] - at(idx, y, z)) > 1e-6f) {
                            std::cerr << "FAIL x value case=" << label
                                      << " request=" << i << " idx=" << idx << "\n";
                            return false;
                        }
            } else if (axis == 'y') {
                if (sl.size() != dx * dz) {
                    std::cerr << "FAIL y size case=" << label << "\n"; return false;
                }
                for (uint64_t x = 0; x < dx; x++)
                    for (uint64_t z = 0; z < dz; z++)
                        if (std::abs(sl[x * dz + z] - at(x, idx, z)) > 1e-6f) {
                            std::cerr << "FAIL y value case=" << label
                                      << " request=" << i << " idx=" << idx << "\n";
                            return false;
                        }
            } else if (axis == 'z') {
                if (sl.size() != dx * dy) {
                    std::cerr << "FAIL z size case=" << label << "\n"; return false;
                }
                for (uint64_t x = 0; x < dx; x++)
                    for (uint64_t y = 0; y < dy; y++)
                        if (std::abs(sl[x * dy + y] - at(x, y, idx)) > 1e-6f) {
                            std::cerr << "FAIL z value case=" << label
                                      << " request=" << i << " idx=" << idx << "\n";
                            return false;
                        }
            } else {
                std::cerr << "FAIL unexpected axis in test\n";
                return false;
            }
        }
        return true;
    };

    const std::vector<std::pair<const char*, std::vector<uint64_t>>> cases = {
        {"same-layer", {0, 1, 2, 7}},
        {"cross-layer", {0, 7, 8, 9, 15, 16}},
        {"duplicate-out-of-order", {7, 0, 7, 2}},
        {"edge", {0, 7, 8}},
        {"empty", {}}
    };

    for (char axis : {'x', 'y', 'z'}) {
        uint64_t dim = axis == 'x' ? dx : axis == 'y' ? dy : dz;
        for (const auto& tc : cases) {
            auto indices = tc.second;
            if (std::string(tc.first) == "edge") indices = {0, dim - 1};
            if (!check_batch(axis, indices, tc.first)) return 1;
        }

        std::vector<uint64_t> stream_indices = {8, 0, 7, 8, dim - 1};
        SliceBatchOptions options;
        options.num_threads = 4;
        options.window_slices = 3;
        std::vector<std::vector<float>> streamed(stream_indices.size());
        std::vector<size_t> completion_order;
        reader.read_slices_batch_stream(axis, stream_indices, options,
            [&](size_t request_pos, uint64_t index, std::vector<float>&& slice) {
                if (request_pos >= stream_indices.size() || index != stream_indices[request_pos]) {
                    std::cerr << "FAIL stream request metadata axis=" << axis << "\n";
                    return;
                }
                completion_order.push_back(request_pos);
                streamed[request_pos] = std::move(slice);
            });
        if (completion_order.size() != stream_indices.size()) {
            std::cerr << "FAIL stream completion count axis=" << axis << "\n";
            return 1;
        }
        for (size_t i = 0; i < stream_indices.size(); i++) {
            auto expected = reader.read_slice(axis, stream_indices[i]);
            if (!allclose(streamed[i], expected)) {
                std::cerr << "FAIL stream value axis=" << axis << " request=" << i << "\n";
                return 1;
            }
        }
        if (!streamed[0].empty() && !streamed[3].empty()) {
            float before = streamed[3][0];
            streamed[0][0] = before + 123.0f;
            if (streamed[3][0] != before) {
                std::cerr << "FAIL duplicate stream buffers share storage axis=" << axis << "\n";
                return 1;
            }
        }
    }

    try {
        (void)reader.read_slices_batch('q', {0}, 4);
        std::cerr << "FAIL invalid axis did not throw\n";
        return 1;
    } catch (const std::invalid_argument&) {
    }
    try {
        (void)reader.read_slices_batch('x', {0, dx}, 4);
        std::cerr << "FAIL out-of-range x did not throw\n";
        return 1;
    } catch (const std::out_of_range&) {
    }
    try {
        (void)reader.read_slices_batch('y', {dy}, 4);
        std::cerr << "FAIL out-of-range y did not throw\n";
        return 1;
    } catch (const std::out_of_range&) {
    }
    try {
        (void)reader.read_slices_batch('z', {dz}, 4);
        std::cerr << "FAIL out-of-range z did not throw\n";
        return 1;
    } catch (const std::out_of_range&) {
    }

    std::cout << "PASS\n";
    return 0;
}

// Regression test for the 0xC0000005 crash in read_slices_batch over
// contiguous adjacent indices on the same axis that map to the SAME
// (axis, block_key). The original sorted_block_list returned a const& into
// the cache_ unordered_map after releasing cache_mutex_; concurrent threads
// on the same block_key entered the slow path together and one would
// overwrite the vector another was iterating -> use-after-free. With by-value
// return this must be stable and correct under heavy contention.
static int test_concurrent_adjacent_same_block_key() {
    std::cout << "  Concurrent adjacent same-axis same-block-key batch...";

    // block_size=8 -> a contiguous run of 8 indices shares one block_key.
    uint64_t bs = 8;
    uint64_t dx = 64, dy = 48, dz = 40;
    uint64_t total = dx * dy * dz;
    std::vector<float> raw_data(total);
    XorShift32 rng(7);
    for (size_t i = 0; i < total; i++) raw_data[i] = rng.next_float();

    auto at = [&](uint64_t x, uint64_t y, uint64_t z) -> float {
        return raw_data[x * dy * dz + y * dz + z];
    };

    fs::path tmp = fs::temp_directory_path() / "block3d_test";
    fs::path raw_path = tmp / "test_conc_adj_raw.dat";
    fs::path b3d_path = tmp / "test_conc_adj.b3d";

    {
        std::ofstream f(raw_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(raw_data.data()),
                raw_data.size() * sizeof(float));
    }
    convert_raw_to_blocked(raw_path.string(), b3d_path.string(),
                           dx, dy, dz, bs, 2, false);

    BlockedFileReader reader(b3d_path.string());

    // Contiguous run within a single block (all map to block_key 0).
    std::vector<uint64_t> same_key_indices;
    for (uint64_t i = 0; i < bs; i++) same_key_indices.push_back(i);

    int hw = static_cast<int>(std::thread::hardware_concurrency());
    if (hw < 16) hw = 16;  // over-subscribe to widen the race window

    const int iterations = 40;
    for (int iter = 0; iter < iterations; iter++) {
        for (char axis : {'x', 'y', 'z'}) {
            // Same-key batch: thread count == batch size forces every thread
            // into the sorted_block_list slow path for one (axis, block_key)
            // simultaneously -- the exact crash scenario.
            auto sk = reader.read_slices_batch(
                axis, same_key_indices, static_cast<int>(same_key_indices.size()));
            if (sk.size() != same_key_indices.size()) {
                std::cerr << "FAIL same-key count axis " << axis << "\n"; return 1;
            }

            // Multi-key contiguous batch: many distinct block_keys with a
            // high thread count, stressing rehash of cache_ and LRU prune
            // while other threads iterate their own (by-value) copies.
            uint64_t dim = (axis == 'x') ? dx : (axis == 'y') ? dy : dz;
            std::vector<uint64_t> mk_idx;
            for (uint64_t i = 0; i < dim; i++) mk_idx.push_back(i);
            auto mk = reader.read_slices_batch(axis, mk_idx, hw);
            if (mk.size() != mk_idx.size()) {
                std::cerr << "FAIL multi-key count axis " << axis << "\n"; return 1;
            }

            // Correctness check on the same-key batch (deterministic).
            for (size_t i = 0; i < same_key_indices.size(); i++) {
                uint64_t idx = same_key_indices[i];
                const auto& sl = sk[i];
                if (axis == 'x') {
                    if (sl.size() != dy * dz) { std::cerr << "FAIL x slice size\n"; return 1; }
                    for (uint64_t y = 0; y < dy; y++)
                        for (uint64_t z = 0; z < dz; z++)
                            if (std::abs(sl[y * dz + z] - at(idx, y, z)) > 1e-6f) {
                                std::cerr << "FAIL x slice val idx=" << idx << "\n"; return 1;
                            }
                } else if (axis == 'y') {
                    if (sl.size() != dx * dz) { std::cerr << "FAIL y slice size\n"; return 1; }
                    for (uint64_t x = 0; x < dx; x++)
                        for (uint64_t z = 0; z < dz; z++)
                            if (std::abs(sl[x * dz + z] - at(x, idx, z)) > 1e-6f) {
                                std::cerr << "FAIL y slice val idx=" << idx << "\n"; return 1;
                            }
                } else {
                    if (sl.size() != dx * dy) { std::cerr << "FAIL z slice size\n"; return 1; }
                    for (uint64_t x = 0; x < dx; x++)
                        for (uint64_t y = 0; y < dy; y++)
                            if (std::abs(sl[x * dy + y] - at(x, y, idx)) > 1e-6f) {
                                std::cerr << "FAIL z slice val idx=" << idx << "\n"; return 1;
                            }
                }
            }
        }
    }

    // Mixed-axis hammer: many threads, all axes, one shared reader/cache.
    // Maximizes cross-key rehash + LRU churn + same-key slow-path races on
    // the single cache_mutex_, with correctness spot-checks.
    {
        int nthreads = hw;
        std::vector<std::thread> ths;
        ths.reserve(nthreads);
        std::atomic<long long> errs{0};
        for (int t = 0; t < nthreads; t++) {
            ths.emplace_back([&, t]() {
                XorShift32 lrng(1000 + t);
                for (int it = 0; it < 300; it++) {
                    char axis = "xyz"[lrng.rand_u64_mod(3)];
                    uint64_t dim = (axis == 'x') ? dx : (axis == 'y') ? dy : dz;
                    uint64_t idx = lrng.rand_u64_mod(dim);
                    auto sl = reader.read_slice(axis, idx);
                    uint64_t exp = (axis == 'x') ? dy * dz
                                 : (axis == 'y') ? dx * dz : dx * dy;
                    if (sl.size() != exp) { errs.fetch_add(1); continue; }
                    uint64_t y = lrng.rand_u64_mod(dy);
                    uint64_t z = lrng.rand_u64_mod(dz);
                    uint64_t x = lrng.rand_u64_mod(dx);
                    float got = 0.0f, want = 0.0f;
                    if (axis == 'x')      { got = sl[y * dz + z]; want = at(idx, y, z); }
                    else if (axis == 'y') { got = sl[x * dz + z]; want = at(x, idx, z); }
                    else                  { got = sl[x * dy + y]; want = at(x, y, idx); }
                    if (std::abs(got - want) > 1e-6f) errs.fetch_add(1);
                }
            });
        }
        for (auto& th : ths) th.join();
        if (errs.load() != 0) {
            std::cerr << "FAIL mixed-axis hammer: " << errs.load() << " errors\n";
            return 1;
        }
    }

    std::cout << "PASS\n";
    return 0;
}

static int test_reader_thread_pool_lifecycle() {
    std::cout << "  Reader persistent thread pool lifecycle...";

    uint64_t dx = 80, dy = 72, dz = 64;
    uint64_t bs = 8;
    uint64_t total = dx * dy * dz;
    std::vector<float> raw_data(total);
    for (uint64_t x = 0; x < dx; x++)
        for (uint64_t y = 0; y < dy; y++)
            for (uint64_t z = 0; z < dz; z++)
                raw_data[x * dy * dz + y * dz + z] =
                    static_cast<float>(x * 1000000 + y * 1000 + z);

    fs::path tmp = fs::temp_directory_path() / "block3d_test";
    fs::path raw_path = tmp / "test_pool_raw.dat";
    fs::path b3d_path = tmp / "test_pool.b3d";

    {
        std::ofstream f(raw_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(raw_data.data()),
                raw_data.size() * sizeof(float));
    }
    convert_raw_to_blocked(raw_path.string(), b3d_path.string(),
                           dx, dy, dz, bs, 4, false);

    BlockedFileReader serial_reader(b3d_path.string(), 1);
    const std::vector<char> axes = {'x', 'y', 'z'};
    const std::vector<uint64_t> x_indices = {0, 7, 8, dx - 1};
    const std::vector<uint64_t> y_indices = {0, 7, 8, dy - 1};
    const std::vector<uint64_t> z_indices = {0, 7, 8, dz - 1};

    auto indices_for = [&](char axis) -> const std::vector<uint64_t>& {
        if (axis == 'x') return x_indices;
        if (axis == 'y') return y_indices;
        return z_indices;
    };

    for (int workers : {1, 2, 4, 16}) {
        for (ReadDispatchStrategy strategy : {ReadDispatchStrategy::RoundRobin,
                                             ReadDispatchStrategy::Contiguous}) {
            BlockedFileReader reader(b3d_path.string(), workers, 0, strategy);
            if (reader.read_dispatch_strategy() != strategy) {
                std::cerr << "FAIL dispatch strategy getter workers=" << workers << "\n";
                return 1;
            }
            if (workers > 1 && reader.thread_pool_workers() != static_cast<size_t>(workers)) {
                std::cerr << "FAIL pool worker count workers=" << workers
                          << " got=" << reader.thread_pool_workers() << "\n";
                return 1;
            }
            for (char axis : axes) {
                for (uint64_t index : indices_for(axis)) {
                    auto expected = serial_reader.read_slice(axis, index);
                    auto got = reader.read_slice(axis, index);
                    if (!allclose(got, expected)) {
                        std::cerr << "FAIL pooled slice axis=" << axis
                                  << " index=" << index
                                  << " workers=" << workers << "\n";
                        return 1;
                    }
                }

                SliceBatchOptions options;
                options.num_threads = workers;
                options.window_slices = 3;
                std::vector<std::vector<float>> got_batch(indices_for(axis).size());
                reader.read_slices_batch_stream(axis, indices_for(axis), options,
                    [&](size_t request_pos, uint64_t, std::vector<float>&& slice) {
                        got_batch[request_pos] = std::move(slice);
                    });
                auto expected_batch = serial_reader.read_slices_batch(axis, indices_for(axis), 1);
                for (size_t i = 0; i < expected_batch.size(); i++) {
                    if (!allclose(got_batch[i], expected_batch[i])) {
                        std::cerr << "FAIL pooled batch axis=" << axis
                                  << " request=" << i
                                  << " workers=" << workers << "\n";
                        return 1;
                    }
                }
            }
            if (workers > 1 && reader.thread_pool_jobs() == 0) {
                std::cerr << "FAIL pool was not used for workers=" << workers << "\n";
                return 1;
            }
        }
    }

    {
        BlockedFileReader reader(b3d_path.string(), 4);
        std::atomic<long long> errs{0};
        std::vector<std::thread> ths;
        for (int t = 0; t < 8; t++) {
            ths.emplace_back([&, t]() {
                for (int it = 0; it < 40; it++) {
                    char axis = axes[static_cast<size_t>((t + it) % 3)];
                    const auto& indices = indices_for(axis);
                    uint64_t idx = indices[static_cast<size_t>(it) % indices.size()];
                    try {
                        if ((it % 2) == 0) {
                            auto got = reader.read_slice(axis, idx);
                            auto expected = serial_reader.read_slice(axis, idx);
                            if (!allclose(got, expected)) errs.fetch_add(1);
                        } else {
                            SliceBatchOptions options;
                            options.num_threads = 4;
                            options.window_slices = 2;
                            std::vector<uint64_t> batch = indices;
                            std::vector<std::vector<float>> got(batch.size());
                            reader.read_slices_batch_stream(axis, batch, options,
                                [&](size_t request_pos, uint64_t, std::vector<float>&& slice) {
                                    got[request_pos] = std::move(slice);
                                });
                            auto expected = serial_reader.read_slices_batch(axis, batch, 1);
                            for (size_t i = 0; i < expected.size(); i++)
                                if (!allclose(got[i], expected[i])) errs.fetch_add(1);
                        }
                    } catch (...) {
                        errs.fetch_add(1);
                    }
                }
            });
        }
        for (auto& th : ths) th.join();
        if (errs.load() != 0) {
            std::cerr << "FAIL concurrent pooled reads: " << errs.load() << " errors\n";
            return 1;
        }
    }

    std::cout << "PASS\n";
    return 0;
}

static int test_batch_column_read() {
    std::cout << "  Batch column read...";

    uint64_t dx = 32, dy = 24, dz = 20;
    uint64_t total = dx * dy * dz;
    std::vector<float> raw_data(total);
    XorShift32 rng(12345);
    for (size_t i = 0; i < total; i++)
        raw_data[i] = rng.next_float();

    auto at = [&](uint64_t x, uint64_t y, uint64_t z) -> float {
        return raw_data[x * dy * dz + y * dz + z];
    };

    fs::path tmp = fs::temp_directory_path() / "block3d_test";
    fs::path raw_path = tmp / "test_batchcol_raw.dat";
    fs::path b3d_path = tmp / "test_batchcol.b3d";

    {
        std::ofstream f(raw_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(raw_data.data()),
                raw_data.size() * sizeof(float));
    }

    convert_raw_to_blocked(raw_path.string(), b3d_path.string(),
                           dx, dy, dz, 8, 4, false);

    BlockedFileReader reader(b3d_path.string());

    std::vector<std::pair<uint64_t, uint64_t>> coords = {
        {2, 3}, {10, 15}, {20, 5}, {dy/3, dz/3}
    };

    {
        auto cols = reader.read_x_columns_batch(coords, 2);
        if (cols.size() != coords.size()) {
            std::cerr << "FAIL X batch column count\n"; return 1;
        }
        for (size_t c = 0; c < coords.size(); c++) {
            auto [y, z] = coords[c];
            if (cols[c].size() != dx) {
                std::cerr << "FAIL X batch col size at " << c << "\n"; return 1;
            }
            for (uint64_t x = 0; x < dx; x++)
                if (std::abs(cols[c][x] - at(x, y, z)) > 1e-6f) {
                    std::cerr << "FAIL X batch col at (" << x << "," << y
                              << "," << z << ")\n";
                    return 1;
                }
        }
    }

    {
        auto cols = reader.read_y_columns_batch(coords, 2);
        if (cols.size() != coords.size()) {
            std::cerr << "FAIL Y batch column count\n"; return 1;
        }
        for (size_t c = 0; c < coords.size(); c++) {
            auto [x, z] = coords[c];
            if (cols[c].size() != dy) {
                std::cerr << "FAIL Y batch col size at " << c << "\n"; return 1;
            }
            for (uint64_t y = 0; y < dy; y++)
                if (std::abs(cols[c][y] - at(x, y, z)) > 1e-6f) {
                    std::cerr << "FAIL Y batch col at (" << x << "," << y
                              << "," << z << ")\n";
                    return 1;
                }
        }
    }

    {
        auto cols = reader.read_z_columns_batch(coords, 2);
        if (cols.size() != coords.size()) {
            std::cerr << "FAIL Z batch column count\n"; return 1;
        }
        for (size_t c = 0; c < coords.size(); c++) {
            auto [x, y] = coords[c];
            if (cols[c].size() != dz) {
                std::cerr << "FAIL Z batch col size at " << c << "\n"; return 1;
            }
            for (uint64_t z = 0; z < dz; z++)
                if (std::abs(cols[c][z] - at(x, y, z)) > 1e-6f) {
                    std::cerr << "FAIL Z batch col at (" << x << "," << y
                              << "," << z << ")\n";
                    return 1;
                }
        }
    }

    std::cout << "PASS\n";
    return 0;
}

static int test_micro_tiled_roundtrip() {
    std::cout << "  Micro-tiled v2 roundtrip...";

    const uint64_t dx = 17, dy = 19, dz = 23;
    const uint64_t bs = 16;
    const uint64_t total = dx * dy * dz;
    std::vector<float> raw_data(total);
    for (uint64_t x = 0; x < dx; x++)
        for (uint64_t y = 0; y < dy; y++)
            for (uint64_t z = 0; z < dz; z++)
                raw_data[x * dy * dz + y * dz + z] =
                    static_cast<float>(x * 1000000 + y * 1000 + z);

    auto at = [&](uint64_t x, uint64_t y, uint64_t z) -> float {
        return raw_data[x * dy * dz + y * dz + z];
    };

    fs::path tmp = fs::temp_directory_path() / "block3d_test";
    fs::path raw_path = tmp / "test_micro_raw.dat";
    fs::path legacy_path = tmp / "test_micro_legacy.b3d";
    fs::path micro_path = tmp / "test_micro_v2.b3d";

    {
        std::ofstream f(raw_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(raw_data.data()),
                raw_data.size() * sizeof(float));
    }

    convert_raw_to_blocked(raw_path.string(), legacy_path.string(),
                           dx, dy, dz, bs, 2, false);
    {
        std::ifstream f(legacy_path, std::ios::binary);
        FileHeader hdr{};
        f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        if (hdr.version != VERSION_LEGACY || hdr.reserved != 0) {
            std::cerr << "FAIL legacy header version/layout\n";
            return 1;
        }
    }

    ConvertOptions options;
    options.block_size = bs;
    options.num_threads = 2;
    options.progress = false;
    options.inner_layout = BlockInnerLayout::MicroTiledXYZ;
    options.micro_size = DEFAULT_MICRO_SIZE;
    convert_raw_to_blocked(raw_path.string(), micro_path.string(),
                           dx, dy, dz, options);

    {
        std::ifstream f(micro_path, std::ios::binary);
        FileHeader hdr{};
        f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        if (hdr.version != VERSION_MICROTILE ||
            header_layout_kind(hdr.reserved) != LAYOUT_MICRO_TILED_XYZ ||
            header_micro_size(hdr.reserved) != DEFAULT_MICRO_SIZE ||
            (hdr.reserved & ~0xffffULL) != 0) {
            std::cerr << "FAIL micro header version/layout\n";
            return 1;
        }
    }

    BlockedFileReader reader(micro_path.string(), 4);
    if (reader.version() != VERSION_MICROTILE ||
        reader.inner_layout() != BlockInnerLayout::MicroTiledXYZ ||
        reader.micro_size() != DEFAULT_MICRO_SIZE) {
        std::cerr << "FAIL reader layout metadata\n";
        return 1;
    }

    for (auto [x, y, z] : std::vector<std::tuple<uint64_t, uint64_t, uint64_t>>{
             {0, 0, 0}, {1, 2, 3}, {8, 9, 10}, {16, 18, 22}}) {
        if (std::abs(reader.read_point(x, y, z) - at(x, y, z)) > 1e-6f) {
            std::cerr << "FAIL micro point at (" << x << "," << y << "," << z << ")\n";
            return 1;
        }
    }

    for (uint64_t x : {0ULL, 8ULL, dx - 1}) {
        auto slice = reader.read_x_slice(x);
        if (slice.size() != dy * dz) { std::cerr << "FAIL micro x size\n"; return 1; }
        for (uint64_t y = 0; y < dy; y++)
            for (uint64_t z = 0; z < dz; z++)
                if (std::abs(slice[y * dz + z] - at(x, y, z)) > 1e-6f) {
                    std::cerr << "FAIL micro x slice\n"; return 1;
                }
    }
    for (uint64_t y : {0ULL, 8ULL, dy - 1}) {
        auto slice = reader.read_y_slice(y);
        if (slice.size() != dx * dz) { std::cerr << "FAIL micro y size\n"; return 1; }
        for (uint64_t x = 0; x < dx; x++)
            for (uint64_t z = 0; z < dz; z++)
                if (std::abs(slice[x * dz + z] - at(x, y, z)) > 1e-6f) {
                    std::cerr << "FAIL micro y slice\n"; return 1;
                }
    }
    for (uint64_t z : {0ULL, 8ULL, dz - 1}) {
        auto slice = reader.read_z_slice(z);
        if (slice.size() != dx * dy) { std::cerr << "FAIL micro z size\n"; return 1; }
        for (uint64_t x = 0; x < dx; x++)
            for (uint64_t y = 0; y < dy; y++)
                if (std::abs(slice[x * dy + y] - at(x, y, z)) > 1e-6f) {
                    std::cerr << "FAIL micro z slice\n"; return 1;
                }
    }

    for (char axis : {'x', 'y', 'z'}) {
        uint64_t dim = axis == 'x' ? dx : axis == 'y' ? dy : dz;
        std::vector<uint64_t> indices = {0, 7, 8, dim - 1};
        auto batch = reader.read_slices_batch(axis, indices, 4);
        for (size_t i = 0; i < indices.size(); i++) {
            auto single = reader.read_slice(axis, indices[i]);
            if (!allclose(batch[i], single)) {
                std::cerr << "FAIL micro batch axis=" << axis << "\n";
                return 1;
            }
        }
        SliceBatchOptions stream_options;
        stream_options.num_threads = 4;
        stream_options.window_slices = 2;
        std::vector<std::vector<float>> streamed(indices.size());
        reader.read_slices_batch_stream(axis, indices, stream_options,
            [&](size_t request_pos, uint64_t, std::vector<float>&& slice) {
                streamed[request_pos] = std::move(slice);
            });
        for (size_t i = 0; i < indices.size(); i++) {
            if (!allclose(streamed[i], batch[i])) {
                std::cerr << "FAIL micro stream axis=" << axis << "\n";
                return 1;
            }
        }
    }

    auto xcol = reader.read_x_column(9, 10);
    auto ycol = reader.read_y_column(8, 10);
    auto zcol = reader.read_z_column(8, 9);
    for (uint64_t x = 0; x < dx; x++)
        if (std::abs(xcol[x] - at(x, 9, 10)) > 1e-6f) { std::cerr << "FAIL micro xcol\n"; return 1; }
    for (uint64_t y = 0; y < dy; y++)
        if (std::abs(ycol[y] - at(8, y, 10)) > 1e-6f) { std::cerr << "FAIL micro ycol\n"; return 1; }
    for (uint64_t z = 0; z < dz; z++)
        if (std::abs(zcol[z] - at(8, 9, z)) > 1e-6f) { std::cerr << "FAIL micro zcol\n"; return 1; }

    auto sub = reader.read_subvolume(1, 17, 2, 19, 3, 23);
    const uint64_t nx = 16, ny = 17, nz = 20;
    if (sub.size() != nx * ny * nz) { std::cerr << "FAIL micro sub size\n"; return 1; }
    for (uint64_t x = 1; x < 17; x++)
        for (uint64_t y = 2; y < 19; y++)
            for (uint64_t z = 3; z < 23; z++) {
                uint64_t idx = (x - 1) * ny * nz + (y - 2) * nz + (z - 3);
                if (std::abs(sub[idx] - at(x, y, z)) > 1e-6f) {
                    std::cerr << "FAIL micro subvolume\n";
                    return 1;
                }
            }

    auto full = reader.read_full_volume();
    if (!allclose(full, raw_data)) {
        std::cerr << "FAIL micro full volume\n";
        return 1;
    }
    if (!reader.verify(raw_path.string(), 500)) {
        std::cerr << "FAIL micro verify\n";
        return 1;
    }

    bool invalid_micro_size_failed = false;
    try {
        ConvertOptions bad = options;
        bad.micro_size = 4;
        convert_raw_to_blocked(raw_path.string(), (tmp / "bad_micro.b3d").string(),
                               dx, dy, dz, bad);
    } catch (const std::invalid_argument&) {
        invalid_micro_size_failed = true;
    }
    if (!invalid_micro_size_failed) {
        std::cerr << "FAIL invalid micro_size accepted\n";
        return 1;
    }

    std::cout << "PASS\n";
    return 0;
}

static int test_memory_budget() {
    std::cout << "  Memory budget parameter...";

    uint64_t dx = 32, dy = 32, dz = 32;
    uint64_t total = dx * dy * dz;
    std::vector<float> raw_data(total);
    XorShift32 rng(99);
    for (size_t i = 0; i < total; i++)
        raw_data[i] = rng.next_float();

    fs::path tmp = fs::temp_directory_path() / "block3d_test";
    fs::path raw_path = tmp / "test_membudget_raw.dat";
    fs::path b3d_path = tmp / "test_membudget.b3d";

    {
        std::ofstream f(raw_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(raw_data.data()),
                raw_data.size() * sizeof(float));
    }

    convert_raw_to_blocked(raw_path.string(), b3d_path.string(),
                           dx, dy, dz, 8, 2, false, 1);

    BlockedFileReader reader(b3d_path.string(), 0, 10);
    if (reader.max_memory_mb() != 10) {
        std::cerr << "FAIL: max_memory_mb not set\n"; return 1;
    }

    auto full = reader.read_full_volume();
    if (full.size() != total) {
        std::cerr << "FAIL: full volume size mismatch\n"; return 1;
    }

    for (size_t i = 0; i < total; i++)
        if (std::abs(full[i] - raw_data[i]) > 1e-6f) {
            std::cerr << "FAIL at index " << i << "\n"; return 1;
        }

    std::cout << "PASS\n";
    return 0;
}

int main() {
    std::cout << "block3d C++ tests\n";
    std::cout << "=================\n";

    fs::path tmp = fs::temp_directory_path() / "block3d_test";
    fs::create_directories(tmp);

    int failed = 0;

    auto run = [&](const char* name, auto fn) {
        std::cout << name << "\n";
        if (fn() != 0) failed++;
    };

    run("Core:", []() {
        return test_format_layout_helpers() | test_morton()
             | test_block_layout() | test_block_order();
    });

    run("IO:", []() {
        return test_roundtrip() | test_slice_reads()
             | test_column_reads() | test_subvolume()
             | test_storage_ratio() | test_verify()
             | test_batch_read() | test_concurrent_adjacent_same_block_key()
             | test_reader_thread_pool_lifecycle()
             | test_batch_column_read()
             | test_micro_tiled_roundtrip()
             | test_memory_budget();
    });

    fs::remove_all(tmp);

    std::cout << "=================\n";
    if (failed > 0) {
        std::cout << failed << " test group(s) FAILED!\n";
        return 1;
    }
    std::cout << "All tests PASSED\n";
    return 0;
}
