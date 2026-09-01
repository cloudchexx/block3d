#include "block3d/core.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

namespace block3d {

// ── Storage medium detection ──────────────────────────────────────────

StorageClass detect_storage_medium(const std::string& output_dir) {
    // Build temp file path inside the target directory.
    std::string tmp_path = output_dir;
    if (!tmp_path.empty() && tmp_path.back() != '/' && tmp_path.back() != '\\')
        tmp_path += '/';
    tmp_path += ".__b3d_storage_test";

    constexpr size_t kTestSize = 1024 * 1024;        // 1 MiB
    std::vector<uint8_t> buf(kTestSize, 0xAA);
    std::vector<double> times;

    for (int round = 0; round < 5; round++) {
        auto t0 = std::chrono::high_resolution_clock::now();

#ifdef _WIN32
        HANDLE h = CreateFileA(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD written = 0;
        WriteFile(h, buf.data(), static_cast<DWORD>(kTestSize), &written,
                  nullptr);
        FlushFileBuffers(h);
        CloseHandle(h);
#else
        int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) continue;
        ::write(fd, buf.data(), kTestSize);
        ::fsync(fd);
        ::close(fd);
#endif

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        times.push_back(ms);
        std::remove(tmp_path.c_str());
    }

    if (times.empty()) return StorageClass::Unknown;

    std::sort(times.begin(), times.end());
    double median = times[times.size() / 2];

    // Empirical thresholds based on fsync latency of a 1 MiB write.
    if (median < 1.0)  return StorageClass::NVMe;
    if (median < 8.0)  return StorageClass::SSD;
    if (median < 120.0) return StorageClass::HDD;
    return StorageClass::Unknown;
}

// ── Adaptive block-size selection ─────────────────────────────────────

uint64_t auto_block_size(uint64_t dim_x, uint64_t dim_y, uint64_t dim_z,
                         StorageClass medium) {
    // Sort dimensions so d_small ≤ d_mid ≤ d_large.
    // The worst slice axis (the smallest dimension) touches
    // ceil(d_mid / bs) × ceil(d_large / bs) blocks.
    uint64_t d[3] = {dim_x, dim_y, dim_z};
    std::sort(d, d + 3);
    uint64_t d_mid   = d[1];
    uint64_t d_large = d[2];

    // Per-medium target: maximum blocks touched by the worst-axis slice.
    // HDD  – seek-dominated; minimise block count (larger bs).
    // SSD  – I/O-bandwidth-dominated; smaller bs wastes less transfer.
    // NVMe / Unknown – balanced.
    uint64_t target_blocks;
    switch (medium) {
    case StorageClass::HDD:     target_blocks = 400;   break;
    case StorageClass::SSD:     target_blocks = 2000;  break;
    case StorageClass::NVMe:    target_blocks = 2000;  break;
    default:                    target_blocks = 1000;  break;
    }

    // Scan upward from 16 in steps of 8; pick the smallest block_size
    // whose worst-axis slice stays within target_blocks.  Step size of 8
    // keeps the block size a multiple of 4 (float alignment).
    uint64_t best = 32;
    for (uint64_t bs = 16; bs <= 256; bs += 8) {
        uint64_t nb_mid   = (d_mid   + bs - 1) / bs;
        uint64_t nb_large = (d_large + bs - 1) / bs;
        if (nb_mid * nb_large <= target_blocks) {
            best = bs;
            break;
        }
    }

    // Enforce minimum blocks per dimension (≥ 8) to avoid degenerate
    // layouts with too few blocks.
    uint64_t max_bs_x = dim_x / 8;
    uint64_t max_bs_y = dim_y / 8;
    uint64_t max_bs_z = dim_z / 8;
    uint64_t max_bs = std::min({max_bs_x, max_bs_y, max_bs_z});
    if (max_bs < 16) max_bs = 16;
    if (best > max_bs) best = max_bs;

    if (best < 16) best = 16;
    if (best > 256) best = 256;

    return best;
}

std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>
BlockLayout3D::block_order() const {
    struct Entry { uint64_t code; uint32_t bx, by_, bz; };
    std::vector<Entry> entries;
    entries.reserve(total_blocks);

    for (uint32_t bx = 0; bx < static_cast<uint32_t>(blocks_x); bx++)
        for (uint32_t by_ = 0; by_ < static_cast<uint32_t>(blocks_y); by_++)
            for (uint32_t bz = 0; bz < static_cast<uint32_t>(blocks_z); bz++)
                entries.push_back({morton_encode(bx, by_, bz), bx, by_, bz});

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.code < b.code; });

    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> result;
    result.reserve(entries.size());
    for (auto& e : entries)
        result.emplace_back(e.bx, e.by_, e.bz);

    return result;
}

} // namespace block3d
