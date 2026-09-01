#include <block3d/block3d.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// 本示例仅使用打包的 C++ 封装头文件（block3d/block3d.hpp）。
// 演示 run_test 风格的流程：原始 float32 .dat → .b3d 转换 → 随机/顺序切片读取 →
// 每个切片写入为无头信息的 float32 .raw 文件。不实现冷/热缓存控制，
// 因此计时反映的是正常 OS 缓存观测值。

/// @brief 轴规格：轴枚举及其字符串名称。
struct AxisSpec {
    block3d::Axis axis;
    const char* name;
};

/// @brief 命令行选项。
struct Options {
    std::string raw_path;
    std::string b3d_path;
    std::string output_dir = "api_example_output_cpp";
    std::uint64_t dim_x = 0;
    std::uint64_t dim_y = 0;
    std::uint64_t dim_z = 0;
    std::uint64_t block_size = 0;
    int threads = 0;
    std::uint64_t memory_limit_mb = 0;
    std::string layout = "legacy";
    std::uint32_t micro_size = 0;
    std::size_t random_count = 100;
    std::size_t seq_count = 10;
    std::size_t batch_window = 1;
    std::uint64_t seed = 42;
    std::uint64_t seq_start = 0;
    std::uint64_t verify_samples = 0;
    bool no_progress = true;
};

/// @brief 存储某个轴/模式下的一次测量结果。
struct CaseResult {
    std::string axis;
    std::string mode;
    std::size_t slices = 0;
    std::uint64_t slice_elems = 0;
    std::uint64_t output_bytes = 0;
    double read_sec = 0.0;
    double write_sec = 0.0;
    double total_sec = 0.0;
    double read_ms_per_slice = 0.0;
    double write_ms_per_slice = 0.0;
    double total_ms_per_slice = 0.0;
    double checksum = 0.0;
};

/// @brief 打印使用信息到 stderr。
void usage(const char* prog) {
    std::cerr
        << u8"用法：" << prog << u8" --raw RAW.dat --b3d OUT.b3d --dim-x N --dim-y N --dim-z N [选项]\n"
        << u8"选项：\n"
        << u8"  --output-dir DIR          输出无头切片 .raw 文件的目录\n"
        << u8"  --layout legacy|micro-tiled  转换布局，默认 legacy\n"
        << u8"  --block-size N            0 = 自动，否则 16..256\n"
        << u8"  --micro-size N            micro-tiled 目前使用 8\n"
        << u8"  --threads N               0 = 自动\n"
        << u8"  --memory-limit N          软内存限制（MiB）\n"
        << u8"  --random-count N          每个轴随机切片数，默认 100\n"
        << u8"  --seq-count N             每个轴顺序切片数，默认 10\n"
        << u8"  --batch-window N          每次 read_slices() 调用读取的切片数，默认 1\n"
        << u8"  --seed N                  随机种子，默认 42\n"
        << u8"  --seq-start N             第一个顺序索引，默认 0\n"
        << u8"  --verify-samples N        转换后对原始文件与 .b3d 进行验证的采样点数量\n"
        << u8"  --progress                显示转换器进度\n";
}

/// @brief 提取下一个命令行参数，并递增索引。
std::string require_value(int argc, char** argv, int& i) {
    if (i + 1 >= argc) throw std::invalid_argument(std::string(u8"缺少值：") + argv[i]);
    return argv[++i];
}

/// @brief 解析命令行参数到 Options 结构体。
Options parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--raw") options.raw_path = require_value(argc, argv, i);
        else if (arg == "--b3d") options.b3d_path = require_value(argc, argv, i);
        else if (arg == "--output-dir") options.output_dir = require_value(argc, argv, i);
        else if (arg == "--dim-x") options.dim_x = std::stoull(require_value(argc, argv, i));
        else if (arg == "--dim-y") options.dim_y = std::stoull(require_value(argc, argv, i));
        else if (arg == "--dim-z") options.dim_z = std::stoull(require_value(argc, argv, i));
        else if (arg == "--block-size") options.block_size = std::stoull(require_value(argc, argv, i));
        else if (arg == "--threads") options.threads = std::stoi(require_value(argc, argv, i));
        else if (arg == "--memory-limit") options.memory_limit_mb = std::stoull(require_value(argc, argv, i));
        else if (arg == "--layout") options.layout = require_value(argc, argv, i);
        else if (arg == "--micro-size") options.micro_size = static_cast<std::uint32_t>(std::stoul(require_value(argc, argv, i)));
        else if (arg == "--random-count") options.random_count = static_cast<std::size_t>(std::stoull(require_value(argc, argv, i)));
        else if (arg == "--seq-count") options.seq_count = static_cast<std::size_t>(std::stoull(require_value(argc, argv, i)));
        else if (arg == "--batch-window") options.batch_window = static_cast<std::size_t>(std::stoull(require_value(argc, argv, i)));
        else if (arg == "--seed") options.seed = std::stoull(require_value(argc, argv, i));
        else if (arg == "--seq-start") options.seq_start = std::stoull(require_value(argc, argv, i));
        else if (arg == "--verify-samples") options.verify_samples = std::stoull(require_value(argc, argv, i));
        else if (arg == "--progress") options.no_progress = false;
        else if (arg == "--help" || arg == "-h") { usage(argv[0]); std::exit(0); }
        else throw std::invalid_argument(u8"未知参数：" + arg);
    }

    if (options.raw_path.empty() || options.b3d_path.empty() ||
        options.dim_x == 0 || options.dim_y == 0 || options.dim_z == 0) {
        usage(argv[0]);
        throw std::invalid_argument(u8"需要 --raw, --b3d, --dim-x, --dim-y 和 --dim-z");
    }
    if (options.batch_window == 0) options.batch_window = 1;
    return options;
}

/// @brief 将布局字符串转换为 block3d::Layout 枚举。
block3d::Layout parse_layout(const std::string& layout) {
    if (layout == "legacy") return block3d::Layout::LegacyXYZ;
    if (layout == "micro-tiled" || layout == "micro_tiled" || layout == "micro") {
        return block3d::Layout::MicroTiledXYZ;
    }
    throw std::invalid_argument(u8"--layout 必须是 legacy 或 micro-tiled");
}

/// @brief 从文件信息中返回指定轴的维度。
std::uint64_t axis_dim(const block3d::FileInfo& info, block3d::Axis axis) {
    switch (axis) {
    case block3d::Axis::X: return info.dim_x;
    case block3d::Axis::Y: return info.dim_y;
    case block3d::Axis::Z: return info.dim_z;
    }
    return 0;
}

/// @brief 生成 @p count 个在 [0, dim-1] 范围内的随机切片索引，使用 @p seed。
std::vector<std::uint64_t> make_random_indices(std::uint64_t dim,
                                                std::size_t count,
                                                std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<std::uint64_t> dist(0, dim - 1);
    std::vector<std::uint64_t> indices;
    indices.reserve(count);
    for (std::size_t i = 0; i < count; i++) indices.push_back(dist(rng));
    return indices;
}

/// @brief 生成 @p count 个从 @p start 开始的连续索引（受 dim 限制）。
std::vector<std::uint64_t> make_sequential_indices(std::uint64_t dim,
                                                    std::size_t count,
                                                    std::uint64_t start) {
    std::vector<std::uint64_t> indices;
    indices.reserve(count);
    for (std::size_t i = 0; i < count && start + i < dim; i++) indices.push_back(start + i);
    return indices;
}

/// @brief 使用给定值的 8 个字节更新 FNV-1a 哈希。
std::uint64_t fnv1a_update(std::uint64_t hash, std::uint64_t value) {
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (int i = 0; i < 8; i++) {
        hash ^= (value >> (i * 8)) & 0xffULL;
        hash *= prime;
    }
    return hash;
}

/// @brief 从索引列表、轴和模式计算确定性十六进制字符串哈希。
std::string plan_hash(const std::vector<std::uint64_t>& indices,
                      const std::string& axis,
                      const std::string& mode) {
    std::uint64_t h = 1469598103934665603ULL;
    for (char c : axis) h = fnv1a_update(h, static_cast<unsigned char>(c));
    for (char c : mode) h = fnv1a_update(h, static_cast<unsigned char>(c));
    for (std::uint64_t index : indices) h = fnv1a_update(h, index);
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << h;
    return oss.str();
}

/// @brief 通过最多采样 64 个均匀间隔的元素计算轻量级校验和。
double sampled_checksum(const std::vector<float>& data) {
    if (data.empty()) return 0.0;
    const std::size_t samples = std::min<std::size_t>(64, data.size());
    const std::size_t stride = std::max<std::size_t>(1, data.size() / samples);
    double sum = 0.0;
    for (std::size_t pos = 0, used = 0; pos < data.size() && used < samples; pos += stride, used++) {
        sum += static_cast<double>(data[pos]);
    }
    sum += static_cast<double>(data.back());
    return sum;
}

/// @brief 构造切片输出文件路径。
fs::path output_path(const fs::path& output_dir,
                     const std::string& axis,
                     const std::string& mode,
                     std::size_t request_pos,
                     std::uint64_t index) {
    std::ostringstream name;
    name << axis << "_" << mode << "_" << std::setw(4) << std::setfill('0') << request_pos
         << "_" << index << ".raw";
    return output_dir / name.str();
}

/// @brief 将浮点数数组写入二进制文件。
void write_slice_file(const fs::path& path, const float* data, std::uint64_t elem_count) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error(u8"无法创建输出切片：" + path.string());
    out.write(reinterpret_cast<const char*>(data),
              static_cast<std::streamsize>(elem_count * sizeof(float)));
    if (!out) throw std::runtime_error(u8"写入输出切片失败：" + path.string());
}

/// @brief 对给定的切片索引列表执行分批读取+写入，并返回计时结果。
CaseResult run_case(block3d::Reader& reader,
                    const AxisSpec& axis,
                    const std::string& mode,
                    const std::vector<std::uint64_t>& indices,
                    std::size_t batch_window,
                    const fs::path& output_dir) {
    CaseResult result;
    result.axis = axis.name;
    result.mode = mode;
    result.slices = indices.size();
    if (indices.empty()) return result;

    const auto case_dir = output_dir / axis.name / mode;
    fs::create_directories(case_dir);

    const auto total_t0 = std::chrono::steady_clock::now();
    for (std::size_t start = 0; start < indices.size(); start += batch_window) {
        const std::size_t end = std::min(indices.size(), start + batch_window);
        const std::vector<std::uint64_t> window(indices.begin() + static_cast<std::ptrdiff_t>(start),
                                               indices.begin() + static_cast<std::ptrdiff_t>(end));

        const auto read_t0 = std::chrono::steady_clock::now();
        const auto batch = reader.read_slices(axis.axis, window);
        const auto read_t1 = std::chrono::steady_clock::now();

        result.slice_elems = batch.slice_elems;
        result.checksum += sampled_checksum(batch.data);

        const auto write_t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < window.size(); i++) {
            const float* slice = batch.data.data() + i * static_cast<std::size_t>(batch.slice_elems);
            write_slice_file(output_path(case_dir, axis.name, mode, start + i, window[i]),
                             slice,
                             batch.slice_elems);
            result.output_bytes += batch.slice_elems * sizeof(float);
        }
        const auto write_t1 = std::chrono::steady_clock::now();

        result.read_sec += std::chrono::duration<double>(read_t1 - read_t0).count();
        result.write_sec += std::chrono::duration<double>(write_t1 - write_t0).count();
    }
    result.total_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - total_t0).count();
    result.read_ms_per_slice = result.slices > 0 ? result.read_sec * 1000.0 / result.slices : 0.0;
    result.write_ms_per_slice = result.slices > 0 ? result.write_sec * 1000.0 / result.slices : 0.0;
    result.total_ms_per_slice = result.slices > 0 ? result.total_sec * 1000.0 / result.slices : 0.0;
    return result;
}

// 将 mode 字符串转换为中文显示名称
std::string mode_cn(const std::string& mode) {
    if (mode == "random") return u8"随机";
    if (mode == "sequential") return u8"顺序";
    return mode;
}

/// @brief 打印单次结果（中文）。
void print_result(const CaseResult& r, const std::string& hash) {
    const double mib = static_cast<double>(r.output_bytes) / (1024.0 * 1024.0);
    const double throughput = r.total_sec > 0.0 ? mib / r.total_sec : 0.0;
    std::cout << u8"API单项成绩 轴=" << r.axis
              << u8" 模式=" << mode_cn(r.mode)
              << u8" 切片数=" << r.slices
              << u8" 每切片元素数=" << r.slice_elems
              << u8" 输出MiB=" << mib
              << u8" 读取总耗时=" << r.read_sec << u8"秒"
              << u8" 写盘总耗时=" << r.write_sec << u8"秒"
              << u8" 总耗时=" << r.total_sec << u8"秒"
              << u8" 平均读取=" << r.read_ms_per_slice << u8"毫秒/片"
              << u8" 平均写盘=" << r.write_ms_per_slice << u8"毫秒/片"
              << u8" 平均总耗时=" << r.total_ms_per_slice << u8"毫秒/片"
              << u8" 吞吐量=" << throughput << u8"MiB/秒"
              << u8" 校验和=" << r.checksum
              << u8" 计划哈希=" << hash << u8"\n";
}

/// @brief 打印所有结果的汇总（中文）。
void print_score_summary(const std::vector<CaseResult>& results) {
    std::cout << u8"\nAPI成绩汇总 单位=毫秒/片；平均总耗时包含读取、写文件和 C++ 循环开销\n";
    const char* modes[] = {"random", "sequential"};
    for (const char* mode : modes) {
        double avg_read = 0.0;
        double avg_write = 0.0;
        double avg_total = 0.0;
        double total_sec = 0.0;
        std::uint64_t output_bytes = 0;
        int count = 0;

        std::ostringstream axes;
        bool first_axis = true;
        for (const char* axis : {"x", "y", "z"}) {
            const auto it = std::find_if(results.begin(), results.end(), [&](const CaseResult& r) {
                return r.mode == mode && r.axis == axis;
            });
            if (it == results.end()) continue;
            if (!first_axis) axes << u8"；";
            first_axis = false;
            axes << axis << u8"轴:平均读取=" << it->read_ms_per_slice
                 << u8",平均写盘=" << it->write_ms_per_slice
                 << u8",平均总耗时=" << it->total_ms_per_slice;
            avg_read += it->read_ms_per_slice;
            avg_write += it->write_ms_per_slice;
            avg_total += it->total_ms_per_slice;
            total_sec += it->total_sec;
            output_bytes += it->output_bytes;
            count++;
        }
        if (count == 0) continue;
        avg_read /= count;
        avg_write /= count;
        avg_total /= count;
        const double total_mib = static_cast<double>(output_bytes) / (1024.0 * 1024.0);
        const double throughput = total_sec > 0.0 ? total_mib / total_sec : 0.0;
        std::cout << u8"API模式汇总 模式=" << mode_cn(mode)
                  << u8" 三轴成绩=[" << axes.str() << u8"]"
                  << u8" 三轴平均读取=" << avg_read << u8"毫秒/片"
                  << u8" 三轴平均写盘=" << avg_write << u8"毫秒/片"
                  << u8" 三轴平均总耗时=" << avg_total << u8"毫秒/片"
                  << u8" 总体吞吐量=" << throughput << u8"MiB/秒\n";
    }
}

} // namespace

/// @brief 程序入口：解析参数、转换、运行随机/顺序测试、打印结果。
int main(int argc, char** argv) {
    try {
        const Options options = parse_args(argc, argv);
        fs::create_directories(options.output_dir);

        block3d::ConvertOptionsFacade conv;
        conv.block_size = options.block_size;
        conv.num_threads = options.threads;
        conv.max_memory_mb = options.memory_limit_mb;
        conv.layout = parse_layout(options.layout);
        conv.micro_size = options.micro_size;
        conv.progress = !options.no_progress;

        // 检查 .b3d 文件是否已存在且各项参数匹配，若匹配则跳过转换
        bool skip_conversion = false;
        if (fs::exists(options.b3d_path)) {
            try {
                block3d::ReaderOptionsFacade temp_reader_options;
                temp_reader_options.num_threads = 1;
                block3d::Reader temp_reader(options.b3d_path, temp_reader_options);
                const auto temp_info = temp_reader.info();
                const block3d::Layout expected_layout = parse_layout(options.layout);
                bool dims_match = (temp_info.dim_x == options.dim_x &&
                                   temp_info.dim_y == options.dim_y &&
                                   temp_info.dim_z == options.dim_z);
                bool block_match = (options.block_size == 0) ||
                                   (temp_info.block_size == options.block_size);
                bool micro_match = (options.micro_size == 0) ||
                                   (temp_info.micro_size == options.micro_size);
                bool layout_match = (temp_info.layout == expected_layout);
                if (dims_match && block_match && micro_match && layout_match) {
                    skip_conversion = true;
                    std::cout << u8"检测到已有匹配的 .b3d 文件，跳过转换。" << std::endl;
                }
            } catch (...) {
                // 无法读取 .b3d 文件，继续转换
            }
        }

        const auto convert_t0 = std::chrono::steady_clock::now();
        if (!skip_conversion) {
            // 执行转换
            block3d::convert_raw_to_b3d(options.raw_path,
                                        options.b3d_path,
                                        options.dim_x,
                                        options.dim_y,
                                        options.dim_z,
                                        conv);
        }
        const auto convert_sec = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - convert_t0).count();

        block3d::ReaderOptionsFacade reader_options;
        reader_options.num_threads = options.threads;
        reader_options.max_memory_mb = options.memory_limit_mb;
        reader_options.read_dispatch = block3d::ReadDispatch::RoundRobin;
        block3d::Reader reader(options.b3d_path, reader_options);
        const auto info = reader.info();

        std::cout << u8"API_CONVERT_RESULT raw=" << options.raw_path
                  << u8" b3d=" << options.b3d_path
                  << u8" dims=" << info.dim_x << "x" << info.dim_y << "x" << info.dim_z
                  << u8" block=" << info.block_size
                  << u8" version=" << info.format_version
                  << u8" layout=" << (info.layout == block3d::Layout::MicroTiledXYZ ? "micro-tiled" : "legacy")
                  << u8" micro_size=" << info.micro_size
                  << u8" convert_sec=" << convert_sec << u8"\n";

        if (options.verify_samples > 0) {
            const auto verify_t0 = std::chrono::steady_clock::now();
            const bool ok = reader.verify(options.raw_path, options.verify_samples, 1e-3f);
            const auto verify_sec = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - verify_t0).count();
            std::cout << u8"API_VERIFY_RESULT ok=" << (ok ? 1 : 0)
                      << u8" samples=" << options.verify_samples
                      << u8" verify_sec=" << verify_sec << u8"\n";
            if (!ok) return 3;
        }

        const AxisSpec axes[] = {
            {block3d::Axis::X, "x"},
            {block3d::Axis::Y, "y"},
            {block3d::Axis::Z, "z"},
        };

        std::vector<CaseResult> results;
        for (const AxisSpec& axis : axes) {
            const std::uint64_t dim = axis_dim(info, axis.axis);
            const auto random_indices = make_random_indices(
                dim, options.random_count, options.seed + static_cast<unsigned char>(axis.name[0]));
            auto random_result = run_case(reader, axis, "random", random_indices,
                                          options.batch_window, options.output_dir);
            print_result(random_result, plan_hash(random_indices, axis.name, "random"));
            results.push_back(random_result);

            const auto sequential_indices = make_sequential_indices(
                dim, options.seq_count, options.seq_start);
            auto sequential_result = run_case(reader, axis, "sequential", sequential_indices,
                                              options.batch_window, options.output_dir);
            print_result(sequential_result, plan_hash(sequential_indices, axis.name, "sequential"));
            results.push_back(sequential_result);
        }

        print_score_summary(results);

        auto x_column = reader.read_column(block3d::Axis::X, 0, 0);
        auto sub = reader.read_subvolume(0, std::min<std::uint64_t>(4, info.dim_x),
                                         0, std::min<std::uint64_t>(4, info.dim_y),
                                         0, std::min<std::uint64_t>(4, info.dim_z));
        std::cout << u8"API_EXTRA_RESULT x_column_len=" << x_column.shape[0]
                  << u8" x_column_first=" << (x_column.data.empty() ? 0.0f : x_column.data.front())
                  << u8" x_column_last=" << (x_column.data.empty() ? 0.0f : x_column.data.back())
                  << u8" subvolume_shape=" << sub.shape[0] << "x" << sub.shape[1] << "x" << sub.shape[2]
                  << u8" point000=" << reader.read_point(0, 0, 0) << u8"\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << u8"错误：" << e.what() << u8"\n";
        return 2;
    }
}
