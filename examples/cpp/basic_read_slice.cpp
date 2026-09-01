#include <block3d/block3d.hpp>

#include <algorithm>
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " FILE.b3d\n";
        return 1;
    }

    block3d::Reader reader(argv[1]);
    auto info = reader.info();
    std::cout << "dims=" << info.dim_x << "x" << info.dim_y << "x" << info.dim_z
              << " block=" << info.block_size
              << " version=" << info.format_version << "\n";

    auto x0 = reader.read_slice(block3d::Axis::X, 0);
    std::cout << "x slice shape=(" << x0.shape[0] << "," << x0.shape[1]
              << ") first=" << (x0.data.empty() ? 0.0f : x0.data[0]) << "\n";

    auto xs = reader.read_slices(block3d::Axis::X, {0, std::min<std::uint64_t>(1, info.dim_x - 1)});
    std::cout << "x batch count=" << xs.slice_count
              << " slice_shape=(" << xs.slice_shape[0] << "," << xs.slice_shape[1] << ")\n";

    auto column = reader.read_column(block3d::Axis::X, 0, 0);
    std::cout << "x column length=" << column.shape[0]
              << " first=" << (column.data.empty() ? 0.0f : column.data[0]) << "\n";

    auto sub = reader.read_subvolume(0, std::min<std::uint64_t>(4, info.dim_x),
                                     0, std::min<std::uint64_t>(4, info.dim_y),
                                     0, std::min<std::uint64_t>(4, info.dim_z));
    std::cout << "subvolume shape=(" << sub.shape[0] << "," << sub.shape[1]
              << "," << sub.shape[2] << ")\n";

    std::cout << "point(0,0,0)=" << reader.read_point(0, 0, 0) << "\n";
    return 0;
}
