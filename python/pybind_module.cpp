#include <block3d/block3d.hpp>
#include <block3d/version.h>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace {

block3d::Axis parse_axis(const std::string& axis) {
    if (axis == "x" || axis == "X") return block3d::Axis::X;
    if (axis == "y" || axis == "Y") return block3d::Axis::Y;
    if (axis == "z" || axis == "Z") return block3d::Axis::Z;
    throw std::invalid_argument("axis must be 'x', 'y', or 'z'");
}

block3d::Layout parse_layout(const std::string& layout) {
    if (layout == "legacy" || layout == "legacy-xyz") return block3d::Layout::LegacyXYZ;
    if (layout == "micro-tiled" || layout == "micro_tiled" || layout == "micro") {
        return block3d::Layout::MicroTiledXYZ;
    }
    throw std::invalid_argument("layout must be 'legacy' or 'micro-tiled'");
}

block3d::ReadDispatch parse_dispatch(const std::string& dispatch) {
    if (dispatch == "round-robin" || dispatch == "round_robin") return block3d::ReadDispatch::RoundRobin;
    if (dispatch == "contiguous") return block3d::ReadDispatch::Contiguous;
    throw std::invalid_argument("read_dispatch must be 'round-robin' or 'contiguous'");
}

std::string layout_name(block3d::Layout layout) {
    return layout == block3d::Layout::MicroTiledXYZ ? "micro-tiled" : "legacy";
}

py::array_t<float> vector_to_array(std::vector<float>&& data,
                                   const std::vector<std::uint64_t>& shape) {
    std::vector<py::ssize_t> py_shape;
    py_shape.reserve(shape.size());
    for (std::uint64_t dim : shape) {
        py_shape.push_back(static_cast<py::ssize_t>(dim));
    }
    py::array_t<float> array(py_shape);
    std::copy(data.begin(), data.end(), static_cast<float*>(array.mutable_data()));
    return array;
}

py::dict info_to_dict(const block3d::FileInfo& info) {
    py::dict result;
    result["dim_x"] = info.dim_x;
    result["dim_y"] = info.dim_y;
    result["dim_z"] = info.dim_z;
    result["block_size"] = info.block_size;
    result["total_blocks"] = info.total_blocks;
    result["data_offset"] = info.data_offset;
    result["format_version"] = info.format_version;
    result["layout"] = layout_name(info.layout);
    result["micro_size"] = info.micro_size;
    return result;
}

} // namespace

PYBIND11_MODULE(block3d, m) {
    m.doc() = "block3d packaged Python API";
    m.attr("__version__") = BLOCK3D_VERSION_STRING;

    m.def("convert_raw_to_b3d",
          [](const std::string& raw_path,
             const std::string& b3d_path,
             std::uint64_t dim_x,
             std::uint64_t dim_y,
             std::uint64_t dim_z,
             std::uint64_t block_size,
             const std::string& layout,
             int threads,
             std::uint64_t memory_limit_mb,
             std::uint32_t micro_size,
             bool progress) {
              block3d::ConvertOptionsFacade options;
              options.block_size = block_size;
              options.num_threads = threads;
              options.max_memory_mb = memory_limit_mb;
              options.layout = parse_layout(layout);
              options.micro_size = micro_size;
              options.progress = progress;
              block3d::convert_raw_to_b3d(raw_path, b3d_path, dim_x, dim_y, dim_z, options);
          },
          py::arg("raw_path"),
          py::arg("b3d_path"),
          py::arg("dim_x"),
          py::arg("dim_y"),
          py::arg("dim_z"),
          py::arg("block_size") = 0,
          py::arg("layout") = "legacy",
          py::arg("threads") = 0,
          py::arg("memory_limit_mb") = 0,
          py::arg("micro_size") = 0,
          py::arg("progress") = true);

    py::class_<block3d::Reader>(m, "Reader")
        .def(py::init([](const std::string& b3d_path,
                         int threads,
                         std::uint64_t memory_limit_mb,
                         const std::string& read_dispatch) {
                 block3d::ReaderOptionsFacade options;
                 options.num_threads = threads;
                 options.max_memory_mb = memory_limit_mb;
                 options.read_dispatch = parse_dispatch(read_dispatch);
                 return std::make_unique<block3d::Reader>(b3d_path, options);
             }),
             py::arg("b3d_path"),
             py::arg("threads") = 0,
             py::arg("memory_limit_mb") = 0,
             py::arg("read_dispatch") = "round-robin")
        .def("info", [](const block3d::Reader& reader) {
            return info_to_dict(reader.info());
        })
        .def("read_slice", [](const block3d::Reader& reader,
                              const std::string& axis,
                              std::uint64_t index) {
            auto array = reader.read_slice(parse_axis(axis), index);
            return vector_to_array(std::move(array.data), array.shape);
        }, py::arg("axis"), py::arg("index"))
        .def("read_slices", [](const block3d::Reader& reader,
                               const std::string& axis,
                               const std::vector<std::uint64_t>& indices) {
            auto batch = reader.read_slices(parse_axis(axis), indices);
            std::vector<std::uint64_t> shape = {batch.slice_count,
                                                batch.slice_shape[0],
                                                batch.slice_shape[1]};
            return vector_to_array(std::move(batch.data), shape);
        }, py::arg("axis"), py::arg("indices"))
        .def("read_column", [](const block3d::Reader& reader,
                               const std::string& axis,
                               std::uint64_t coord1,
                               std::uint64_t coord2) {
            auto array = reader.read_column(parse_axis(axis), coord1, coord2);
            return vector_to_array(std::move(array.data), array.shape);
        }, py::arg("axis"), py::arg("coord1"), py::arg("coord2"))
        .def("read_subvolume", [](const block3d::Reader& reader,
                                  std::uint64_t xs,
                                  std::uint64_t xe,
                                  std::uint64_t ys,
                                  std::uint64_t ye,
                                  std::uint64_t zs,
                                  std::uint64_t ze) {
            auto array = reader.read_subvolume(xs, xe, ys, ye, zs, ze);
            return vector_to_array(std::move(array.data), array.shape);
        }, py::arg("xs"), py::arg("xe"),
           py::arg("ys"), py::arg("ye"),
           py::arg("zs"), py::arg("ze"))
        .def("read_point", &block3d::Reader::read_point,
             py::arg("x"), py::arg("y"), py::arg("z"))
        .def("verify", &block3d::Reader::verify,
             py::arg("raw_path"),
             py::arg("samples") = 1000,
             py::arg("tolerance") = 1e-3f);
}
