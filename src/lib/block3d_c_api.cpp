#include "block3d/block3d.h"

#include "block3d/converter.hpp"
#include "block3d/reader.hpp"
#include "block3d/version.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

struct block3d_context {
    block3d::BlockedFileReader reader;

    explicit block3d_context(const char* path, const block3d_reader_options& options)
        : reader(path,
                 options.num_threads,
                 options.max_memory_mb,
                 options.read_dispatch == BLOCK3D_READ_DISPATCH_CONTIGUOUS
                     ? block3d::ReadDispatchStrategy::Contiguous
                     : block3d::ReadDispatchStrategy::RoundRobin) {}
};

namespace {

char to_reader_axis(block3d_axis axis) {
    switch (axis) {
    case BLOCK3D_AXIS_X: return 'x';
    case BLOCK3D_AXIS_Y: return 'y';
    case BLOCK3D_AXIS_Z: return 'z';
    }
    throw std::invalid_argument("invalid axis");
}

block3d_layout to_c_layout(block3d::BlockInnerLayout layout) {
    return layout == block3d::BlockInnerLayout::MicroTiledXYZ
        ? BLOCK3D_LAYOUT_MICRO_TILED_XYZ
        : BLOCK3D_LAYOUT_LEGACY_XYZ;
}

block3d_status status_from_exception() {
    try {
        throw;
    } catch (const std::invalid_argument&) {
        return BLOCK3D_ERROR_INVALID_ARGUMENT;
    } catch (const std::out_of_range&) {
        return BLOCK3D_ERROR_OUT_OF_RANGE;
    } catch (const std::bad_alloc&) {
        return BLOCK3D_ERROR_OUT_OF_MEMORY;
    } catch (const std::runtime_error& e) {
        const std::string message = e.what();
        if (message.find("magic") != std::string::npos ||
            message.find("version") != std::string::npos ||
            message.find("layout") != std::string::npos ||
            message.find("header") != std::string::npos) {
            return BLOCK3D_ERROR_FORMAT;
        }
        if (message.find("file") != std::string::npos ||
            message.find("open") != std::string::npos ||
            message.find("create") != std::string::npos ||
            message.find("mmap") != std::string::npos ||
            message.find("map") != std::string::npos) {
            return BLOCK3D_ERROR_IO;
        }
        return BLOCK3D_ERROR_INTERNAL;
    } catch (...) {
        return BLOCK3D_ERROR_INTERNAL;
    }
}

void clear_array(block3d_array* array) {
    if (!array) return;
    array->data = nullptr;
    array->dim0 = 0;
    array->dim1 = 0;
    array->dim2 = 0;
    array->ndim = 0;
    array->count = 0;
}

void clear_batch(block3d_slice_batch* batch) {
    if (!batch) return;
    batch->data = nullptr;
    batch->indices = nullptr;
    batch->slice_count = 0;
    batch->slice_elems = 0;
    batch->dim0 = 0;
    batch->dim1 = 0;
    batch->axis = BLOCK3D_AXIS_X;
}

void fill_array(block3d_array* out_array,
                std::vector<float>&& data,
                uint64_t dim0,
                uint64_t dim1,
                uint64_t dim2,
                size_t ndim) {
    clear_array(out_array);
    out_array->count = static_cast<uint64_t>(data.size());
    out_array->dim0 = dim0;
    out_array->dim1 = dim1;
    out_array->dim2 = dim2;
    out_array->ndim = ndim;
    if (data.empty()) return;
    out_array->data = new float[data.size()];
    std::copy(data.begin(), data.end(), out_array->data);
}

std::pair<uint64_t, uint64_t> slice_dims(const block3d::BlockedFileReader& reader,
                                         block3d_axis axis) {
    switch (axis) {
    case BLOCK3D_AXIS_X: return {reader.dim_y(), reader.dim_z()};
    case BLOCK3D_AXIS_Y: return {reader.dim_x(), reader.dim_z()};
    case BLOCK3D_AXIS_Z: return {reader.dim_x(), reader.dim_y()};
    }
    throw std::invalid_argument("invalid axis");
}

} // namespace

extern "C" {

block3d_version block3d_get_version(void) {
    return block3d_version{BLOCK3D_VERSION_MAJOR, BLOCK3D_VERSION_MINOR, BLOCK3D_VERSION_PATCH};
}

const char* block3d_status_message(block3d_status status) {
    switch (status) {
    case BLOCK3D_OK: return "ok";
    case BLOCK3D_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case BLOCK3D_ERROR_IO: return "I/O error";
    case BLOCK3D_ERROR_OUT_OF_MEMORY: return "out of memory";
    case BLOCK3D_ERROR_OUT_OF_RANGE: return "out of range";
    case BLOCK3D_ERROR_FORMAT: return "format error";
    case BLOCK3D_ERROR_INTERNAL: return "internal error";
    }
    return "unknown status";
}

block3d_convert_options block3d_default_convert_options(void) {
    block3d_convert_options options;
    options.block_size = 0;
    options.num_threads = 0;
    options.max_memory_mb = 0;
    options.layout = BLOCK3D_LAYOUT_LEGACY_XYZ;
    options.micro_size = 0;
    options.progress = 1;
    return options;
}

block3d_reader_options block3d_default_reader_options(void) {
    block3d_reader_options options;
    options.num_threads = 0;
    options.max_memory_mb = 0;
    options.read_dispatch = BLOCK3D_READ_DISPATCH_ROUND_ROBIN;
    return options;
}

block3d_status block3d_convert_raw_to_b3d(
    const char* raw_path,
    const char* b3d_path,
    uint64_t dim_x,
    uint64_t dim_y,
    uint64_t dim_z,
    const block3d_convert_options* options) {
    if (!raw_path || !b3d_path || dim_x == 0 || dim_y == 0 || dim_z == 0) {
        return BLOCK3D_ERROR_INVALID_ARGUMENT;
    }
    try {
        block3d_convert_options c_options = options ? *options : block3d_default_convert_options();
        block3d::ConvertOptions convert_options;
        convert_options.block_size = c_options.block_size;
        if (convert_options.block_size == 0) {
            std::filesystem::path output_path(b3d_path);
            std::filesystem::path output_dir = output_path.parent_path();
            if (output_dir.empty()) output_dir = ".";
            convert_options.block_size = block3d::auto_block_size(
                dim_x, dim_y, dim_z, block3d::detect_storage_medium(output_dir.string()));
        }
        convert_options.num_threads = c_options.num_threads;
        convert_options.max_memory_mb = c_options.max_memory_mb;
        convert_options.progress = c_options.progress != 0;
        if (c_options.layout == BLOCK3D_LAYOUT_MICRO_TILED_XYZ) {
            convert_options.inner_layout = block3d::BlockInnerLayout::MicroTiledXYZ;
            convert_options.micro_size = c_options.micro_size == 0
                ? block3d::DEFAULT_MICRO_SIZE
                : c_options.micro_size;
        } else if (c_options.layout == BLOCK3D_LAYOUT_LEGACY_XYZ) {
            convert_options.inner_layout = block3d::BlockInnerLayout::LegacyXYZ;
            convert_options.micro_size = 0;
        } else {
            return BLOCK3D_ERROR_INVALID_ARGUMENT;
        }
        block3d::convert_raw_to_blocked(raw_path, b3d_path, dim_x, dim_y, dim_z, convert_options);
        return BLOCK3D_OK;
    } catch (...) {
        return status_from_exception();
    }
}

block3d_status block3d_open_b3d(
    const char* b3d_path,
    const block3d_reader_options* options,
    block3d_context** out_context) {
    if (!b3d_path || !out_context) return BLOCK3D_ERROR_INVALID_ARGUMENT;
    *out_context = nullptr;
    try {
        block3d_reader_options reader_options = options ? *options : block3d_default_reader_options();
        if (reader_options.read_dispatch != BLOCK3D_READ_DISPATCH_ROUND_ROBIN &&
            reader_options.read_dispatch != BLOCK3D_READ_DISPATCH_CONTIGUOUS) {
            return BLOCK3D_ERROR_INVALID_ARGUMENT;
        }
        *out_context = new block3d_context(b3d_path, reader_options);
        return BLOCK3D_OK;
    } catch (...) {
        *out_context = nullptr;
        return status_from_exception();
    }
}

void block3d_close(block3d_context* context) {
    delete context;
}

block3d_status block3d_get_info(
    const block3d_context* context,
    block3d_file_info* out_info) {
    if (!context || !out_info) return BLOCK3D_ERROR_INVALID_ARGUMENT;
    try {
        const auto& reader = context->reader;
        out_info->dim_x = reader.dim_x();
        out_info->dim_y = reader.dim_y();
        out_info->dim_z = reader.dim_z();
        out_info->block_size = reader.block_size();
        out_info->total_blocks = reader.total_blocks();
        out_info->data_offset = reader.data_offset();
        out_info->format_version = reader.version();
        out_info->layout = to_c_layout(reader.inner_layout());
        out_info->micro_size = reader.micro_size();
        return BLOCK3D_OK;
    } catch (...) {
        return status_from_exception();
    }
}

block3d_status block3d_read_slice(
    block3d_context* context,
    block3d_axis axis,
    uint64_t index,
    block3d_array* out_array) {
    if (!context || !out_array) return BLOCK3D_ERROR_INVALID_ARGUMENT;
    clear_array(out_array);
    try {
        auto dims = slice_dims(context->reader, axis);
        auto data = context->reader.read_slice(to_reader_axis(axis), index);
        fill_array(out_array, std::move(data), dims.first, dims.second, 0, 2);
        return BLOCK3D_OK;
    } catch (...) {
        clear_array(out_array);
        return status_from_exception();
    }
}

block3d_status block3d_read_slices_batch(
    block3d_context* context,
    block3d_axis axis,
    const uint64_t* indices,
    uint64_t index_count,
    block3d_slice_batch* out_batch) {
    if (!context || !out_batch || (index_count > 0 && !indices)) {
        return BLOCK3D_ERROR_INVALID_ARGUMENT;
    }
    clear_batch(out_batch);
    try {
        auto dims = slice_dims(context->reader, axis);
        std::vector<uint64_t> requested(indices, indices + index_count);
        auto slices = context->reader.read_slices_batch(to_reader_axis(axis), requested);
        const uint64_t slice_elems = dims.first * dims.second;
        out_batch->slice_count = index_count;
        out_batch->slice_elems = slice_elems;
        out_batch->dim0 = dims.first;
        out_batch->dim1 = dims.second;
        out_batch->axis = axis;
        if (index_count > 0) {
            out_batch->indices = new uint64_t[index_count];
            std::copy(requested.begin(), requested.end(), out_batch->indices);
            out_batch->data = new float[static_cast<size_t>(index_count * slice_elems)];
            for (uint64_t i = 0; i < index_count; i++) {
                std::copy(slices[static_cast<size_t>(i)].begin(),
                          slices[static_cast<size_t>(i)].end(),
                          out_batch->data + static_cast<size_t>(i * slice_elems));
            }
        }
        return BLOCK3D_OK;
    } catch (...) {
        block3d_free_slice_batch(out_batch);
        return status_from_exception();
    }
}

block3d_status block3d_read_column(
    block3d_context* context,
    block3d_axis axis,
    uint64_t coord1,
    uint64_t coord2,
    block3d_array* out_array) {
    if (!context || !out_array) return BLOCK3D_ERROR_INVALID_ARGUMENT;
    clear_array(out_array);
    try {
        std::vector<float> data;
        uint64_t length = 0;
        switch (axis) {
        case BLOCK3D_AXIS_X:
            data = context->reader.read_x_column(coord1, coord2);
            length = context->reader.dim_x();
            break;
        case BLOCK3D_AXIS_Y:
            data = context->reader.read_y_column(coord1, coord2);
            length = context->reader.dim_y();
            break;
        case BLOCK3D_AXIS_Z:
            data = context->reader.read_z_column(coord1, coord2);
            length = context->reader.dim_z();
            break;
        default:
            return BLOCK3D_ERROR_INVALID_ARGUMENT;
        }
        fill_array(out_array, std::move(data), length, 0, 0, 1);
        return BLOCK3D_OK;
    } catch (...) {
        clear_array(out_array);
        return status_from_exception();
    }
}

block3d_status block3d_read_subvolume(
    block3d_context* context,
    uint64_t xs,
    uint64_t xe,
    uint64_t ys,
    uint64_t ye,
    uint64_t zs,
    uint64_t ze,
    block3d_array* out_array) {
    if (!context || !out_array) return BLOCK3D_ERROR_INVALID_ARGUMENT;
    clear_array(out_array);
    try {
        auto data = context->reader.read_subvolume(xs, xe, ys, ye, zs, ze);
        fill_array(out_array, std::move(data), xe - xs, ye - ys, ze - zs, 3);
        return BLOCK3D_OK;
    } catch (...) {
        clear_array(out_array);
        return status_from_exception();
    }
}

block3d_status block3d_read_point(
    block3d_context* context,
    uint64_t x,
    uint64_t y,
    uint64_t z,
    float* out_value) {
    if (!context || !out_value) return BLOCK3D_ERROR_INVALID_ARGUMENT;
    try {
        *out_value = context->reader.read_point(x, y, z);
        return BLOCK3D_OK;
    } catch (...) {
        return status_from_exception();
    }
}

block3d_status block3d_verify_points(
    block3d_context* context,
    const char* raw_path,
    uint64_t samples,
    float tolerance) {
    if (!context || !raw_path) return BLOCK3D_ERROR_INVALID_ARGUMENT;
    try {
        return context->reader.verify(raw_path, samples, tolerance)
            ? BLOCK3D_OK
            : BLOCK3D_ERROR_INTERNAL;
    } catch (...) {
        return status_from_exception();
    }
}

void block3d_free_array(block3d_array* array) {
    if (!array) return;
    delete[] array->data;
    clear_array(array);
}

void block3d_free_slice_batch(block3d_slice_batch* batch) {
    if (!batch) return;
    delete[] batch->data;
    delete[] batch->indices;
    clear_batch(batch);
}

} // extern "C"
