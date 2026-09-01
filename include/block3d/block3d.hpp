#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace block3d {

struct Version {
    int major;
    int minor;
    int patch;
};

enum class Axis { X, Y, Z };
enum class Layout { LegacyXYZ, MicroTiledXYZ };
enum class ReadDispatch { RoundRobin, Contiguous };

struct ConvertOptionsFacade {
    std::uint64_t block_size = 0;
    int num_threads = 0;
    std::uint64_t max_memory_mb = 0;
    Layout layout = Layout::LegacyXYZ;
    std::uint32_t micro_size = 0;
    bool progress = true;
};

struct ReaderOptionsFacade {
    int num_threads = 0;
    std::uint64_t max_memory_mb = 0;
    ReadDispatch read_dispatch = ReadDispatch::RoundRobin;
};

struct FileInfo {
    std::uint64_t dim_x;
    std::uint64_t dim_y;
    std::uint64_t dim_z;
    std::uint64_t block_size;
    std::uint64_t total_blocks;
    std::uint64_t data_offset;
    std::uint32_t format_version;
    Layout layout;
    std::uint32_t micro_size;
};

struct Array {
    std::vector<float> data;
    std::vector<std::uint64_t> shape;
};

struct SliceBatch {
    std::vector<float> data;
    std::vector<std::uint64_t> indices;
    Axis axis;
    std::uint64_t slice_count;
    std::uint64_t slice_elems;
    std::vector<std::uint64_t> slice_shape;
};

Version version();

void convert_raw_to_b3d(
    const std::string& raw_path,
    const std::string& b3d_path,
    std::uint64_t dim_x,
    std::uint64_t dim_y,
    std::uint64_t dim_z,
    const ConvertOptionsFacade& options = {});

class Reader {
public:
    explicit Reader(const std::string& b3d_path,
                    const ReaderOptionsFacade& options = {});
    ~Reader();

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&&) noexcept;
    Reader& operator=(Reader&&) noexcept;

    FileInfo info() const;

    Array read_slice(Axis axis, std::uint64_t index) const;
    SliceBatch read_slices(Axis axis, const std::vector<std::uint64_t>& indices) const;
    Array read_column(Axis axis, std::uint64_t coord1, std::uint64_t coord2) const;
    Array read_subvolume(std::uint64_t xs, std::uint64_t xe,
                         std::uint64_t ys, std::uint64_t ye,
                         std::uint64_t zs, std::uint64_t ze) const;
    float read_point(std::uint64_t x, std::uint64_t y, std::uint64_t z) const;
    bool verify(const std::string& raw_path,
                std::uint64_t samples = 1000,
                float tolerance = 1e-3f) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace block3d
