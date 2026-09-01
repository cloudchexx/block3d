#include "block3d/block3d.hpp"

#include "block3d/converter.hpp"
#include "block3d/reader.hpp"
#include "block3d/version.h"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace block3d {

namespace {

char to_reader_axis(Axis axis) {
    switch (axis) {
    case Axis::X: return 'x';
    case Axis::Y: return 'y';
    case Axis::Z: return 'z';
    }
    return 'x';
}

BlockInnerLayout to_inner_layout(Layout layout) {
    return layout == Layout::MicroTiledXYZ
        ? BlockInnerLayout::MicroTiledXYZ
        : BlockInnerLayout::LegacyXYZ;
}

Layout to_facade_layout(BlockInnerLayout layout) {
    return layout == BlockInnerLayout::MicroTiledXYZ
        ? Layout::MicroTiledXYZ
        : Layout::LegacyXYZ;
}

ReadDispatchStrategy to_reader_dispatch(ReadDispatch dispatch) {
    return dispatch == ReadDispatch::Contiguous
        ? ReadDispatchStrategy::Contiguous
        : ReadDispatchStrategy::RoundRobin;
}

std::vector<std::uint64_t> slice_shape(const BlockedFileReader& reader, Axis axis) {
    switch (axis) {
    case Axis::X: return {reader.dim_y(), reader.dim_z()};
    case Axis::Y: return {reader.dim_x(), reader.dim_z()};
    case Axis::Z: return {reader.dim_x(), reader.dim_y()};
    }
    return {};
}

} // namespace

Version version() {
    return {BLOCK3D_VERSION_MAJOR, BLOCK3D_VERSION_MINOR, BLOCK3D_VERSION_PATCH};
}

void convert_raw_to_b3d(
    const std::string& raw_path,
    const std::string& b3d_path,
    std::uint64_t dim_x,
    std::uint64_t dim_y,
    std::uint64_t dim_z,
    const ConvertOptionsFacade& options) {
    ConvertOptions convert_options;
    convert_options.block_size = options.block_size;
    if (convert_options.block_size == 0) {
        std::filesystem::path output_path(b3d_path);
        std::filesystem::path output_dir = output_path.parent_path();
        if (output_dir.empty()) output_dir = ".";
        convert_options.block_size = auto_block_size(
            dim_x, dim_y, dim_z, detect_storage_medium(output_dir.string()));
    }
    convert_options.num_threads = options.num_threads;
    convert_options.progress = options.progress;
    convert_options.max_memory_mb = options.max_memory_mb;
    convert_options.inner_layout = to_inner_layout(options.layout);
    convert_options.micro_size = options.layout == Layout::MicroTiledXYZ
        ? (options.micro_size == 0 ? DEFAULT_MICRO_SIZE : options.micro_size)
        : 0;
    convert_raw_to_blocked(raw_path, b3d_path, dim_x, dim_y, dim_z, convert_options);
}

class Reader::Impl {
public:
    Impl(const std::string& b3d_path, const ReaderOptionsFacade& options)
        : reader(b3d_path,
                 options.num_threads,
                 options.max_memory_mb,
                 to_reader_dispatch(options.read_dispatch)) {}

    BlockedFileReader reader;
};

Reader::Reader(const std::string& b3d_path, const ReaderOptionsFacade& options)
    : impl_(std::make_unique<Impl>(b3d_path, options)) {}

Reader::~Reader() = default;
Reader::Reader(Reader&&) noexcept = default;
Reader& Reader::operator=(Reader&&) noexcept = default;

FileInfo Reader::info() const {
    const auto& reader = impl_->reader;
    return FileInfo{
        reader.dim_x(),
        reader.dim_y(),
        reader.dim_z(),
        reader.block_size(),
        reader.total_blocks(),
        reader.data_offset(),
        reader.version(),
        to_facade_layout(reader.inner_layout()),
        reader.micro_size()};
}

Array Reader::read_slice(Axis axis, std::uint64_t index) const {
    auto data = impl_->reader.read_slice(to_reader_axis(axis), index);
    return Array{std::move(data), slice_shape(impl_->reader, axis)};
}

SliceBatch Reader::read_slices(Axis axis, const std::vector<std::uint64_t>& indices) const {
    auto slices = impl_->reader.read_slices_batch(to_reader_axis(axis), indices);
    auto shape = slice_shape(impl_->reader, axis);
    std::uint64_t slice_elems = shape[0] * shape[1];
    std::vector<float> data;
    data.reserve(slice_elems * slices.size());
    for (const auto& slice : slices) {
        data.insert(data.end(), slice.begin(), slice.end());
    }
    return SliceBatch{std::move(data), indices, axis,
                      static_cast<std::uint64_t>(slices.size()),
                      slice_elems,
                      std::move(shape)};
}

Array Reader::read_column(Axis axis, std::uint64_t coord1, std::uint64_t coord2) const {
    std::vector<float> data;
    std::uint64_t length = 0;
    switch (axis) {
    case Axis::X:
        data = impl_->reader.read_x_column(coord1, coord2);
        length = impl_->reader.dim_x();
        break;
    case Axis::Y:
        data = impl_->reader.read_y_column(coord1, coord2);
        length = impl_->reader.dim_y();
        break;
    case Axis::Z:
        data = impl_->reader.read_z_column(coord1, coord2);
        length = impl_->reader.dim_z();
        break;
    }
    return Array{std::move(data), {length}};
}

Array Reader::read_subvolume(std::uint64_t xs, std::uint64_t xe,
                             std::uint64_t ys, std::uint64_t ye,
                             std::uint64_t zs, std::uint64_t ze) const {
    auto data = impl_->reader.read_subvolume(xs, xe, ys, ye, zs, ze);
    return Array{std::move(data), {xe - xs, ye - ys, ze - zs}};
}

float Reader::read_point(std::uint64_t x, std::uint64_t y, std::uint64_t z) const {
    return impl_->reader.read_point(x, y, z);
}

bool Reader::verify(const std::string& raw_path,
                    std::uint64_t samples,
                    float tolerance) const {
    return impl_->reader.verify(raw_path, samples, tolerance);
}

} // namespace block3d
