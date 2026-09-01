#include <block3d/block3d.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

float value_at(std::uint64_t x, std::uint64_t y, std::uint64_t z) {
    return static_cast<float>(x * 1000000ULL + y * 1000ULL + z);
}

void write_raw(const std::string& path, std::uint64_t dx, std::uint64_t dy, std::uint64_t dz) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot create raw");
    for (std::uint64_t x = 0; x < dx; x++) {
        for (std::uint64_t y = 0; y < dy; y++) {
            for (std::uint64_t z = 0; z < dz; z++) {
                float value = value_at(x, y, z);
                out.write(reinterpret_cast<const char*>(&value), sizeof(value));
            }
        }
    }
}

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

bool nearly_equal(float a, float b) {
    return std::abs(a - b) <= 1e-6f;
}

} // namespace

int main() {
    try {
        const std::uint64_t dx = 9, dy = 10, dz = 11;
        const std::string raw_path = "block3d_cpp_api_raw.dat";
        const std::string b3d_path = "block3d_cpp_api.b3d";
        std::remove(raw_path.c_str());
        std::remove(b3d_path.c_str());

        write_raw(raw_path, dx, dy, dz);

        block3d::ConvertOptionsFacade conv;
        conv.block_size = 16;
        conv.num_threads = 2;
        conv.progress = false;
        block3d::convert_raw_to_b3d(raw_path, b3d_path, dx, dy, dz, conv);

        block3d::ReaderOptionsFacade reader_options;
        reader_options.num_threads = 2;
        block3d::Reader reader(b3d_path, reader_options);
        auto info = reader.info();
        require(info.dim_x == dx && info.dim_y == dy && info.dim_z == dz, "info dims");
        require(info.layout == block3d::Layout::LegacyXYZ, "info layout");

        auto x_slice = reader.read_slice(block3d::Axis::X, 3);
        require((x_slice.shape == std::vector<std::uint64_t>{dy, dz}), "x slice shape");
        require(nearly_equal(x_slice.data[4 * dz + 5], value_at(3, 4, 5)), "x slice value");

        auto batch = reader.read_slices(block3d::Axis::Y, {0, 2, 4});
        require(batch.slice_count == 3, "batch count");
        require(batch.slice_elems == dx * dz, "batch elems");
        require((batch.slice_shape == std::vector<std::uint64_t>{dx, dz}), "batch shape");
        require(nearly_equal(batch.data[batch.slice_elems + 6 * dz + 7], value_at(6, 2, 7)), "batch value");

        auto col = reader.read_column(block3d::Axis::X, 4, 5);
        require((col.shape == std::vector<std::uint64_t>{dx}), "column shape");
        require(nearly_equal(col.data[8], value_at(8, 4, 5)), "column value");

        auto sub = reader.read_subvolume(2, 6, 3, 8, 4, 10);
        require((sub.shape == std::vector<std::uint64_t>{4, 5, 6}), "sub shape");
        std::uint64_t sub_idx = (3 - 2) * 5 * 6 + (5 - 3) * 6 + (7 - 4);
        require(nearly_equal(sub.data[sub_idx], value_at(3, 5, 7)), "sub value");

        require(nearly_equal(reader.read_point(8, 9, 10), value_at(8, 9, 10)), "point");
        require(reader.verify(raw_path, 100, 1e-3f), "verify");

        bool threw = false;
        try {
            (void)reader.read_slice(block3d::Axis::Z, dz);
        } catch (const std::out_of_range&) {
            threw = true;
        }
        require(threw, "out of range throws");

        std::remove(raw_path.c_str());
        std::remove(b3d_path.c_str());
        std::cout << "CPP_API_TEST_RESULT ok=1\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << "\n";
        return 1;
    }
}
