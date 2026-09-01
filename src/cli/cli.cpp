#include "block3d/reader.hpp"
#include "block3d/converter.hpp"
#include "block3d/rng.hpp"
#include "benchmark_cache/benchmark_cache.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cctype>

using namespace block3d;

static void print_usage(const char* prog) {
    std::cerr << "Usage:\n";
    std::cerr << "  " << prog << " convert <input> <output> --dim-x N --dim-y N --dim-z N\n";
    std::cerr << "         [--block-size N]  (16–256; 0 or omit = auto-detect via storage probe)\n";
    std::cerr << "         [--threads N] [--memory-limit N] [--layout legacy|micro-tiled] [--micro-size N] [--no-progress]\n";
    std::cerr << "  " << prog << " info <file>\n";
    std::cerr << "  " << prog << " cache-prepare <file> --size auto|N[GB] [--cold-scrub-ratio R] [--overwrite]\n";
    std::cerr << "  " << prog << " bench <file> [--num-reads N] [--random] [--threads N] [--memory-limit N]\n";
    std::cerr << "         [--batch-read legacy|fused] [--batch-window N]\n";
    std::cerr << "         [--pipeline off] [--read-dispatch round-robin|contiguous]\n";
    std::cerr << "         [--cache-mode cold|hot|both] [--warm-up]\n";
    std::cerr << "         [--cold-method scrub|none] [--cold-scrub-file FILE] [--cold-scrub-ratio R]\n";
    std::cerr << "         [--cold-scrub-passes N] [--cold-settle-ms N] [--cold-isolation suite|case]\n";
    std::cerr << "         [--warmup-scope dataset|workload]\n";
    std::cerr << "  " << prog << " verify <b3d> <raw> [--samples N]\n";
    std::cerr << "  " << prog << " extract <file> --axis x|y|z --index N [-o FILE]\n";
    std::cerr << "  " << prog << " extract <file> --column-axis x|y|z --c1 N --c2 N [-o FILE]\n";
}

static bool has_arg(int argc, char* argv[], const char* name) {
    for (int i = 0; i < argc; i++)
        if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}

static const char* get_arg(int argc, char* argv[], const char* name,
                            const char* def = nullptr) {
    for (int i = 1; i < argc - 1; i++)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return def;
}

static int get_arg_int(int argc, char* argv[], const char* name, int def) {
    auto* s = get_arg(argc, argv, name);
    return s ? std::atoi(s) : def;
}

static double get_arg_double(int argc, char* argv[], const char* name, double def) {
    auto* s = get_arg(argc, argv, name);
    return s ? std::atof(s) : def;
}

static uint64_t get_arg_uint64(int argc, char* argv[], const char* name,
                                uint64_t def) {
    auto* s = get_arg(argc, argv, name);
    return s ? std::strtoull(s, nullptr, 10) : def;
}

static uint64_t axis_dim(const BlockedFileReader& reader, char axis) {
    if (axis == 'x') return reader.dim_x();
    if (axis == 'y') return reader.dim_y();
    if (axis == 'z') return reader.dim_z();
    throw std::invalid_argument("Unknown axis: " + str_axis(axis));
}

static std::string normalize_dispatch(const std::string& value) {
    return value == "round-robin" ? "round_robin" : value;
}

static ReadDispatchStrategy parse_dispatch_strategy(const std::string& value) {
    return value == "contiguous"
        ? ReadDispatchStrategy::Contiguous
        : ReadDispatchStrategy::RoundRobin;
}

static std::string hex_u64(uint64_t value) {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << value;
    return oss.str();
}

// ── Command: convert ─────────────────────────────────────────────────

static int cmd_convert(int argc, char* argv[]) {
    if (argc < 4) { std::cerr << "Missing arguments for convert\n"; return 1; }
    const char* input  = argv[2];
    const char* output = argv[3];
    uint64_t dx = get_arg_uint64(argc, argv, "--dim-x", 0);
    uint64_t dy = get_arg_uint64(argc, argv, "--dim-y", 0);
    uint64_t dz = get_arg_uint64(argc, argv, "--dim-z", 0);
    if (dx == 0 || dy == 0 || dz == 0) {
        std::cerr << "Must specify --dim-x, --dim-y, --dim-z\n"; return 1;
    }
    uint64_t bs = get_arg_uint64(argc, argv, "--block-size", 0);
    int nt = get_arg_int(argc, argv, "--threads", 0);
    uint64_t mem_mb = get_arg_uint64(argc, argv, "--memory-limit", 0);
    bool prog = !has_arg(argc, argv, "--no-progress");
    std::string layout_name = get_arg(argc, argv, "--layout", "legacy");
    BlockInnerLayout inner_layout = BlockInnerLayout::LegacyXYZ;
    uint32_t micro_size = 0;
    if (layout_name == "legacy") {
        inner_layout = BlockInnerLayout::LegacyXYZ;
        micro_size = static_cast<uint32_t>(get_arg_uint64(argc, argv, "--micro-size", 0));
        if (micro_size != 0) {
            std::cerr << "--layout legacy requires --micro-size 0 or omitted\n";
            return 1;
        }
    } else if (layout_name == "micro-tiled") {
        inner_layout = BlockInnerLayout::MicroTiledXYZ;
        micro_size = static_cast<uint32_t>(get_arg_uint64(argc, argv, "--micro-size", DEFAULT_MICRO_SIZE));
        if (micro_size != DEFAULT_MICRO_SIZE) {
            std::cerr << "--layout micro-tiled requires --micro-size 8\n";
            return 1;
        }
    } else {
        std::cerr << "Invalid layout: " << layout_name << " (must be legacy or micro-tiled)\n";
        return 1;
    }

    // Auto-detect storage medium and pick optimal block_size.
    if (bs == 0) {
        std::string out_str(output);
        auto slash = out_str.find_last_of("/\\");
        std::string parent =
            (slash != std::string::npos) ? out_str.substr(0, slash) : ".";
        auto sc = detect_storage_medium(parent);
        bs = auto_block_size(dx, dy, dz, sc);
        const char* sc_names[] = {"HDD", "SSD", "NVMe", "Unknown"};
        std::cout << "[AUTO] Detected "
                  << sc_names[static_cast<int>(sc)]
                  << " storage, auto block_size=" << bs << "\n";
    }
    if (bs < 16 || bs > 256) {
        std::cerr << "Invalid block_size: " << bs
                  << " (must be 16–256, or 0 for auto)\n";
        return 1;
    }
    if (inner_layout == BlockInnerLayout::MicroTiledXYZ && bs % micro_size != 0) {
        std::cerr << "block_size must be divisible by micro_size\n";
        return 1;
    }

    std::cout << "Converting: " << input << " -> " << output << "\n";
    std::cout << "  dims=" << dx << "x" << dy << "x" << dz
              << " block_size=" << bs << " threads="
              << (nt ? std::to_string(nt) : "auto")
              << " layout=" << layout_name;
    if (inner_layout == BlockInnerLayout::MicroTiledXYZ) {
        std::cout << " micro_size=" << micro_size;
    }
    if (mem_mb) std::cout << " mem_limit=" << mem_mb << "MB";
    std::cout << "\n";

    ConvertOptions options;
    options.block_size = bs;
    options.num_threads = nt;
    options.progress = prog;
    options.max_memory_mb = mem_mb;
    options.inner_layout = inner_layout;
    options.micro_size = micro_size;

    auto t0 = std::chrono::high_resolution_clock::now();
    convert_raw_to_blocked(input, output, dx, dy, dz, options);
    auto t1 = std::chrono::high_resolution_clock::now();
    double dt = std::chrono::duration<double>(t1 - t0).count();

    std::ifstream out_f(output, std::ios::binary | std::ios::ate);
    auto size_bytes = out_f.tellg();
    double size_gb = size_bytes / (1024.0 * 1024.0 * 1024.0);
    std::cout << "Done in " << dt << "s. Output: " << size_gb << " GB\n";
    return 0;
}

// ── Command: info ────────────────────────────────────────────────────

static int cmd_info(int argc, char* argv[]) {
    if (argc < 3) { std::cerr << "Missing file argument\n"; return 1; }
    BlockedFileReader reader(argv[2]);
    const auto& lay = reader.layout();
    double raw_size = static_cast<double>(reader.dim_x() * reader.dim_y()
                                          * reader.dim_z() * 4);

    std::ifstream f(argv[2], std::ios::binary | std::ios::ate);
    double file_size = static_cast<double>(f.tellg());

    std::cout << "File:          " << argv[2] << "\n";
    std::cout << "Dimensions:    X=" << reader.dim_x()
              << " Y=" << reader.dim_y()
              << " Z=" << reader.dim_z() << "\n";
    std::cout << "Version:       " << reader.version() << "\n";
    std::cout << "Block size:    " << reader.block_size() << "\n";
    std::cout << "Layout:        "
              << (reader.inner_layout() == BlockInnerLayout::MicroTiledXYZ
                  ? "micro-tiled" : "legacy") << "\n";
    std::cout << "Micro size:    " << reader.micro_size() << "\n";
    std::cout << "Total blocks:  " << reader.total_blocks() << "\n";
    std::cout << "Data offset:   " << reader.data_offset() << "\n";
    std::cout << "Blocks layout: " << lay.blocks_x << "x" << lay.blocks_y
              << "x" << lay.blocks_z << "\n";
    std::cout << "Storage ratio: " << (file_size / raw_size) << "x\n";
    return 0;
}

// ── Command: cache-prepare ────────────────────────────────────────────

static int cmd_cache_prepare(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Missing file argument for cache-prepare\n";
        return 1;
    }
    CachePrepareOptions options;
    options.path = argv[2];
    options.cold_scrub_ratio = get_arg_double(argc, argv, "--cold-scrub-ratio", 1.50);
    options.overwrite = has_arg(argc, argv, "--overwrite");

    const char* size_arg = get_arg(argc, argv, "--size", nullptr);
    const char* size_gb_arg = get_arg(argc, argv, "--size-gb", nullptr);
    if (size_arg && size_gb_arg) {
        std::cerr << "--size and --size-gb are mutually exclusive\n";
        return 1;
    }
    if (size_arg) {
        std::string size_text = size_arg;
        if (size_text == "auto") {
            options.size_bytes = required_scrub_bytes(options.cold_scrub_ratio);
        } else {
            bool ok = false;
            options.size_bytes = parse_size_bytes(size_text, ok);
            if (!ok || options.size_bytes == 0) {
                std::cerr << "Invalid --size value: " << size_text << "\n";
                return 1;
            }
        }
    } else if (size_gb_arg) {
        bool ok = false;
        options.size_bytes = parse_size_bytes(std::string(size_gb_arg) + "GB", ok);
        if (!ok || options.size_bytes == 0) {
            std::cerr << "Invalid --size-gb value: " << size_gb_arg << "\n";
            return 1;
        }
    } else {
        std::cerr << "cache-prepare requires --size auto|N[GB] or --size-gb N\n";
        return 1;
    }

    std::cout << "CACHE_PREPARE path=" << options.path << "\n";
    std::cout << "CACHE_PREPARE bytes=" << options.size_bytes
              << " ram_ratio=" << options.cold_scrub_ratio << "\n";
    auto result = prepare_cache_scrub_file(options);
    if (!result.ok) {
        std::cerr << "CACHE_PREPARE_RESULT ok=0 message=\"" << result.message << "\"\n";
        return 1;
    }
    std::cout << "CACHE_PREPARE_RESULT ok=1 reused=" << (result.reused ? 1 : 0)
              << " bytes=" << result.bytes
              << " elapsed_sec=" << result.elapsed_sec
              << " checksum=" << result.checksum
              << " message=\"" << result.message << "\"\n";
    return 0;
}

// ── Command: bench ───────────────────────────────────────────────────

struct BenchCasePlan {
    char axis = 'x';
    bool random_mode = false;
    std::vector<uint64_t> indices;
    uint64_t plan_hash = 0;
};

struct BenchPhaseResult {
    std::string cache;
    char axis = 'x';
    double total_sec = 0.0;
    double avg_ms = 0.0;
    double throughput = 0.0;
    size_t count = 0;
};

static uint64_t compute_plan_hash(const BenchCasePlan& plan,
                                  uint64_t dx, uint64_t dy, uint64_t dz,
                                  int num_reads, int threads) {
    uint64_t h = 1469598103934665603ULL;
    h = fnv1a64_update(h, &plan.axis, sizeof(plan.axis));
    h = fnv1a64_u64(h, plan.random_mode ? 1 : 0);
    h = fnv1a64_u64(h, dx);
    h = fnv1a64_u64(h, dy);
    h = fnv1a64_u64(h, dz);
    h = fnv1a64_u64(h, static_cast<uint64_t>(num_reads));
    h = fnv1a64_u64(h, static_cast<uint64_t>(threads));
    for (uint64_t idx : plan.indices) h = fnv1a64_u64(h, idx);
    return h;
}

static std::vector<BenchCasePlan> build_bench_plans(const BlockedFileReader& reader,
                                                     int num_reads,
                                                     bool random_mode,
                                                     int threads) {
    std::vector<BenchCasePlan> plans;
    XorShift32 rng(42);
    for (char axis : {'x', 'y', 'z'}) {
        uint64_t dim = axis_dim(reader, axis);
        BenchCasePlan plan;
        plan.axis = axis;
        plan.random_mode = random_mode;
        if (random_mode) {
            for (int i = 0; i < num_reads; i++)
                plan.indices.push_back(rng.rand_u64_mod(dim));
        } else {
            uint64_t step = std::max<uint64_t>(1, dim / static_cast<uint64_t>(num_reads));
            for (uint64_t i = 0; i < dim && plan.indices.size() < static_cast<size_t>(num_reads); i += step)
                plan.indices.push_back(i);
        }
        plan.plan_hash = compute_plan_hash(plan, reader.dim_x(), reader.dim_y(),
                                           reader.dim_z(), num_reads, threads);
        plans.push_back(std::move(plan));
    }
    return plans;
}

static BenchPhaseResult run_read_phase(const char* path,
                                       const BenchCasePlan& plan,
                                       const std::string& cache_name,
                                       int threads,
                                       uint64_t mem_mb,
                                       const std::string& batch_read,
                                       size_t batch_window_slices,
                                       ReadDispatchStrategy dispatch_strategy) {
    BlockedFileReader reader(path, threads, mem_mb, dispatch_strategy);
    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<std::vector<float>> slices;
    if (batch_read == "legacy") {
        for (uint64_t index : plan.indices) {
            slices.push_back(reader.read_slice(plan.axis, index));
        }
    } else {
        SliceBatchOptions options;
        options.num_threads = threads;
        options.window_slices = batch_window_slices;
        slices.resize(plan.indices.size());
        reader.read_slices_batch_stream(plan.axis, plan.indices, options,
            [&](size_t request_pos, uint64_t, std::vector<float>&& slice) {
                slices[request_pos] = std::move(slice);
            });
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    BenchPhaseResult result;
    result.cache = cache_name;
    result.axis = plan.axis;
    result.total_sec = std::chrono::duration<double>(t1 - t0).count();
    result.count = slices.size();
    if (result.count > 0 && result.total_sec > 0.0) {
        result.avg_ms = result.total_sec / static_cast<double>(result.count) * 1000.0;
        result.throughput = static_cast<double>(result.count) / result.total_sec;
    }
    std::string mode = plan.random_mode ? "random" : "sequential";
    std::cout << "BENCHMARK_RESULT cache=" << cache_name
              << " mode=" << mode
              << " axis=" << plan.axis
              << " total_sec=" << result.total_sec
              << " avg_ms=" << result.avg_ms
              << " slices_per_sec=" << result.throughput
              << " count=" << result.count
              << " reader_pool_workers=" << reader.thread_pool_workers()
              << " reader_pool_jobs=" << reader.thread_pool_jobs()
              << " reader_pool_serial_fallbacks=" << reader.thread_pool_serial_fallbacks()
              << " plan_hash=" << hex_u64(plan.plan_hash) << "\n";
    std::cout << "  " << (plan.random_mode ? "Random" : "Sequential") << " "
              << static_cast<char>(std::toupper(static_cast<unsigned char>(plan.axis)))
              << "-slice (" << result.count << "x, cache=" << cache_name << "): "
              << result.total_sec << "s total, " << result.avg_ms << " ms avg, "
              << result.throughput << " slices/s\n";
    return result;
}

static void warm_up_for_plans(const char* path,
                              const std::vector<BenchCasePlan>& plans,
                              WarmupScope scope,
                              int threads,
                              uint64_t mem_mb,
                              const std::string& batch_read,
                              size_t batch_window_slices,
                              ReadDispatchStrategy dispatch_strategy) {
    auto t0 = std::chrono::high_resolution_clock::now();
    uint64_t logical_bytes = 0;
    uint64_t checksum = 0;
    {
        BlockedFileReader reader(path, threads, mem_mb, dispatch_strategy);
        if (scope == WarmupScope::Dataset) {
            uint64_t bs = reader.block_size();
            logical_bytes = reader.total_blocks() * bs * bs * bs * sizeof(float);
            reader.warm_up(false, query_system_page_size(), mem_mb);
        } else {
            for (const auto& plan : plans) {
                std::vector<std::vector<float>> slices;
                if (batch_read == "legacy") {
                    slices.reserve(plan.indices.size());
                    for (uint64_t index : plan.indices) {
                        slices.push_back(reader.read_slice(plan.axis, index));
                    }
                } else {
                    SliceBatchOptions options;
                    options.num_threads = threads;
                    options.window_slices = batch_window_slices;
                    slices.resize(plan.indices.size());
                    reader.read_slices_batch_stream(plan.axis, plan.indices, options,
                        [&](size_t request_pos, uint64_t, std::vector<float>&& slice) {
                            slices[request_pos] = std::move(slice);
                        });
                }
                for (const auto& slice : slices) {
                    logical_bytes += slice.size() * sizeof(float);
                    if (!slice.empty()) {
                        checksum ^= static_cast<uint64_t>(slice.front() * 1000003.0f);
                        checksum ^= static_cast<uint64_t>(slice.back() * 9176.0f);
                    }
                }
            }
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "WARMUP_RESULT scope=" << to_string(scope)
              << " elapsed_sec=" << elapsed
              << " bytes=" << logical_bytes
              << " checksum=" << checksum << "\n";
}

static bool do_scrub_if_needed(const CacheOptions& cache_options) {
    if (cache_options.cold_method == ColdMethod::None) {
        std::cout << "CACHE_SCRUB method=none\n";
        std::cout << "CACHE_VALIDITY state=cold_first_touch\n";
        return true;
    }
    std::cout << "[Phase] Cache scrub\n";
    auto scrub = run_cache_scrub(cache_options);
    std::cout << "CACHE_SCRUB method=unrelated_file_sweep\n";
    std::cout << "CACHE_SCRUB file=" << scrub.path << "\n";
    std::cout << "CACHE_SCRUB bytes=" << scrub.bytes_read
              << " file_bytes=" << scrub.file_bytes
              << " required_bytes=" << scrub.required_bytes
              << " ram_ratio=" << scrub.ram_ratio
              << " passes=" << scrub.passes << "\n";
    std::cout << "CACHE_SCRUB elapsed_sec=" << scrub.elapsed_sec
              << " checksum=" << scrub.checksum << "\n";
    std::cout << "CACHE_VALIDITY state=" << scrub.message << "\n";
    if (!scrub.ok) {
        std::cerr << "Cache scrub failed: " << scrub.message << "\n";
        return false;
    }
    return true;
}

static int cmd_bench(int argc, char* argv[]) {
    if (argc < 3) { std::cerr << "Missing file argument\n"; return 1; }
    const char* path = argv[2];
    int num_reads = get_arg_int(argc, argv, "--num-reads", 100);
    bool random_mode = has_arg(argc, argv, "--random");
    int nt = get_arg_int(argc, argv, "--threads", 0);
    uint64_t mem_mb = get_arg_uint64(argc, argv, "--memory-limit", 0);
    bool warm_up_flag = has_arg(argc, argv, "--warm-up");
    std::string batch_read = get_arg(argc, argv, "--batch-read", "fused");
    int batch_window_arg = get_arg_int(argc, argv, "--batch-window", 4);
    std::string pipeline = get_arg(argc, argv, "--pipeline", "off");
    std::string read_dispatch = get_arg(argc, argv, "--read-dispatch", "round-robin");

    if (num_reads <= 0) {
        std::cerr << "--num-reads must be > 0\n";
        return 1;
    }
    if (batch_read != "legacy" && batch_read != "fused") {
        std::cerr << "Invalid --batch-read: " << batch_read << " (must be legacy|fused)\n";
        return 1;
    }
    if (batch_window_arg <= 0) {
        std::cerr << "--batch-window must be > 0\n";
        return 1;
    }
    if (pipeline != "off") {
        std::cerr << "Invalid --pipeline for read-only bench: " << pipeline
                  << " (only off is supported; use run_test for write pipeline benchmarking)\n";
        return 1;
    }
    if (read_dispatch != "round-robin" && read_dispatch != "contiguous") {
        std::cerr << "Invalid --read-dispatch: " << read_dispatch
                  << " (must be round-robin|contiguous)\n";
        return 1;
    }
    ReadDispatchStrategy dispatch_strategy = parse_dispatch_strategy(read_dispatch);

    CacheOptions cache_options;
    if (const char* s = get_arg(argc, argv, "--cache-mode", nullptr)) {
        if (!parse_cache_mode(s, cache_options.mode)) {
            std::cerr << "Invalid --cache-mode: " << s << " (must be cold|hot|both)\n";
            return 1;
        }
    }
    if (warm_up_flag) {
        if (has_arg(argc, argv, "--cache-mode") && cache_options.mode != CacheMode::Hot) {
            std::cerr << "--warm-up is a compatibility alias for --cache-mode hot and conflicts with cold/both\n";
            return 1;
        }
        cache_options.mode = CacheMode::Hot;
    }
    if (const char* s = get_arg(argc, argv, "--cold-method", nullptr)) {
        if (!parse_cold_method(s, cache_options.cold_method)) {
            std::cerr << "Invalid --cold-method: " << s << " (must be scrub|none)\n";
            return 1;
        }
    }
    if (const char* s = get_arg(argc, argv, "--cold-isolation", nullptr)) {
        if (!parse_cold_isolation(s, cache_options.cold_isolation)) {
            std::cerr << "Invalid --cold-isolation: " << s << " (must be suite|case)\n";
            return 1;
        }
    }
    if (const char* s = get_arg(argc, argv, "--warmup-scope", nullptr)) {
        if (!parse_warmup_scope(s, cache_options.warmup_scope)) {
            std::cerr << "Invalid --warmup-scope: " << s << " (must be dataset|workload)\n";
            return 1;
        }
    }
    cache_options.cold_scrub_file = get_arg(argc, argv, "--cold-scrub-file", "");
    cache_options.cold_scrub_ratio = get_arg_double(argc, argv, "--cold-scrub-ratio", 1.50);
    int scrub_passes = get_arg_int(argc, argv, "--cold-scrub-passes", 1);
    int settle_ms = get_arg_int(argc, argv, "--cold-settle-ms", 1000);
    if (scrub_passes <= 0 || settle_ms < 0) {
        std::cerr << "--cold-scrub-passes must be > 0 and --cold-settle-ms must be >= 0\n";
        return 1;
    }
    cache_options.cold_scrub_passes = static_cast<uint32_t>(scrub_passes);
    cache_options.cold_settle_ms = static_cast<uint32_t>(settle_ms);

    if (cache_options.cold_scrub_ratio <= 0.0) {
        std::cerr << "--cold-scrub-ratio must be > 0\n";
        return 1;
    }
    if (cache_mode_has_cold(cache_options.mode) &&
        cache_options.cold_method == ColdMethod::Scrub &&
        cache_options.cold_scrub_file.empty()) {
        std::cerr << "--cold-scrub-file is required when cold scrub is enabled\n";
        return 1;
    }

    BlockedFileReader plan_reader(path, nt, mem_mb);
    auto plans = build_bench_plans(plan_reader, num_reads, random_mode,
                                   plan_reader.num_threads());

    std::cout << "Benchmarking: " << path << "\n";
    std::cout << "Dimensions: X=" << plan_reader.dim_x() << " Y=" << plan_reader.dim_y()
              << " Z=" << plan_reader.dim_z() << "\n";
    std::cout << "Threads: " << plan_reader.num_threads() << "\n";
    std::cout << "CACHE_CONFIG mode=" << to_string(cache_options.mode)
              << " cold_method=" << to_string(cache_options.cold_method)
              << " cold_isolation=" << to_string(cache_options.cold_isolation)
              << " warmup_scope=" << to_string(cache_options.warmup_scope)
              << " timing_scope=read_only\n";
    std::cout << "BATCH_READ mode=" << batch_read
              << " window_slices=" << batch_window_arg << "\n";
    std::cout << "PIPELINE mode=" << pipeline
              << " buffer_mb=0 window_slices=0 queue_slices=0 writer_threads=0\n";
    std::cout << "READ_DISPATCH strategy=" << normalize_dispatch(read_dispatch) << "\n";
    std::cout << "TIMING_SCOPE value=read_only\n";
    std::cout << "PLAN_HASH_SUITE value=";
    uint64_t suite_hash = 1469598103934665603ULL;
    for (const auto& plan : plans) suite_hash = fnv1a64_u64(suite_hash, plan.plan_hash);
    std::cout << hex_u64(suite_hash) << "\n";
    for (const auto& plan : plans) {
        std::cout << "PLAN_HASH axis=" << plan.axis
                  << " mode=" << (plan.random_mode ? "random" : "sequential")
                  << " value=" << hex_u64(plan.plan_hash)
                  << " count=" << plan.indices.size() << "\n";
    }
    std::cout << "\n";

    std::vector<BenchPhaseResult> cold_results;
    std::vector<BenchPhaseResult> hot_results;

    if (cache_mode_has_cold(cache_options.mode)) {
        std::cout << "CACHE_PHASE name="
                  << (cache_options.cold_method == ColdMethod::Scrub ? "cold_scrubbed" : "cold_first_touch")
                  << "\n";
        if (cache_options.cold_isolation == ColdIsolation::Suite) {
            if (!do_scrub_if_needed(cache_options)) return 1;
            for (const auto& plan : plans) {
                cold_results.push_back(run_read_phase(path, plan,
                    cache_options.cold_method == ColdMethod::Scrub ? "cold_scrubbed" : "cold_first_touch",
                    nt, mem_mb, batch_read, static_cast<size_t>(batch_window_arg),
                    dispatch_strategy));
            }
        } else {
            for (const auto& plan : plans) {
                if (!do_scrub_if_needed(cache_options)) return 1;
                cold_results.push_back(run_read_phase(path, plan,
                    cache_options.cold_method == ColdMethod::Scrub ? "cold_scrubbed" : "cold_first_touch",
                    nt, mem_mb, batch_read, static_cast<size_t>(batch_window_arg),
                    dispatch_strategy));
            }
        }
    }

    if (cache_mode_has_hot(cache_options.mode)) {
        std::cout << "\n[Phase] Warm-up\n";
        warm_up_for_plans(path, plans, cache_options.warmup_scope, nt, mem_mb,
                          batch_read, static_cast<size_t>(batch_window_arg),
                          dispatch_strategy);
        std::cout << "CACHE_PHASE name=hot_prefetched\n";
        for (const auto& plan : plans) {
            hot_results.push_back(run_read_phase(path, plan, "hot_prefetched", nt, mem_mb, batch_read, static_cast<size_t>(batch_window_arg), dispatch_strategy));
        }
    }

    if (!cold_results.empty() && !hot_results.empty()) {
        for (const auto& cold : cold_results) {
            auto it = std::find_if(hot_results.begin(), hot_results.end(),
                                   [&](const BenchPhaseResult& hot) {
                                       return hot.axis == cold.axis;
                                   });
            if (it != hot_results.end() && it->total_sec > 0.0) {
                std::cout << "BENCHMARK_COMPARE mode="
                          << (random_mode ? "random" : "sequential")
                          << " axis=" << cold.axis
                          << " observed_ratio=" << (cold.total_sec / it->total_sec)
                          << "\n";
            }
        }
    }

    return 0;
}

// ── Command: verify ──────────────────────────────────────────────────

static int cmd_verify(int argc, char* argv[]) {
    // Argument order is <b3d> <raw>: the blocked file is the first positional
    // argument, consistent with every other subcommand (info/bench/extract all
    // take the .b3d first). The raw file is the second positional argument and
    // is only needed for cross-checking. Earlier the order was <raw> <b3d>,
    // which was inconsistent and caused users to swap the files; opening the
    // raw file as a .b3d then threw an uncaught exception that aborted the
    // process via Windows fast-fail (0xC0000409) with no output.
    if (argc < 4) { std::cerr << "Usage: verify <b3d> <raw> [--samples N]\n"; return 1; }
    const char* b3d_path = argv[2];
    const char* raw_path = argv[3];
    int samples = get_arg_int(argc, argv, "--samples", 10000);

    BlockedFileReader reader(b3d_path);
    std::cout << "Verifying " << samples << " random points...\n";
    auto t0 = std::chrono::high_resolution_clock::now();
    bool ok = reader.verify(raw_path, samples);
    auto t1 = std::chrono::high_resolution_clock::now();
    double dt = std::chrono::duration<double>(t1 - t0).count();

    if (ok) {
        std::cout << "PASSED in " << dt << "s\n";
        return 0;
    } else {
        std::cout << "FAILED! Data mismatch detected.\n";
        return 1;
    }
}

// ── Command: extract ─────────────────────────────────────────────────

static int cmd_extract(int argc, char* argv[]) {
    if (argc < 3) { std::cerr << "Missing file argument\n"; return 1; }
    const char* path = argv[2];
    const char* output = get_arg(argc, argv, "-o", nullptr);
    const char* axis = get_arg(argc, argv, "--axis", nullptr);
    const char* col_axis = get_arg(argc, argv, "--column-axis", nullptr);

    BlockedFileReader reader(path);

    auto write_raw = [&](const std::vector<float>& data,
                         const std::string& def_name) {
        std::string out_path = output ? output : def_name;
        std::ofstream f(out_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()),
                data.size() * sizeof(float));
        std::cout << "Saved to: " << out_path << " ("
                  << data.size() << " floats)\n";
    };

    if (axis) {
        int idx = get_arg_int(argc, argv, "--index", -1);
        if (idx < 0) { std::cerr << "Missing --index\n"; return 1; }
        auto data = reader.read_slice(axis[0], static_cast<uint64_t>(idx));
        write_raw(data, "slice_" + str_axis(axis[0]) + std::to_string(idx) + ".raw");
    } else if (col_axis) {
        int c1 = get_arg_int(argc, argv, "--c1", -1);
        int c2 = get_arg_int(argc, argv, "--c2", -1);
        if (c1 < 0 || c2 < 0) { std::cerr << "Missing --c1 and --c2\n"; return 1; }
        std::vector<float> data;
        if (col_axis[0] == 'x')
            data = reader.read_x_column(c1, c2);
        else if (col_axis[0] == 'y')
            data = reader.read_y_column(c1, c2);
        else
            data = reader.read_z_column(c1, c2);
        write_raw(data, "col_" + str_axis(col_axis[0])
                  + "_" + std::to_string(c1) + "_" + std::to_string(c2) + ".raw");
    } else {
        std::cerr << "Specify --axis or --column-axis\n"; return 1;
    }
    return 0;
}

// ── Main ─────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    std::string cmd = argv[1];

    // All subcommands can throw std::runtime_error / std::out_of_range
    // (file not found, invalid magic, dimension out of range, mmap failure,
    // ...). Without a catch, such exceptions are uncaught -> std::terminate
    // -> on Windows a fast-fail (0xC0000409 "stack buffer overrun") with no
    // output, which looks like a crash rather than a clean error. Catch here
    // and report the message with a non-zero exit code so bad input (e.g.
    // wrong argument order / missing file / corrupt file) fails gracefully.
    try {
        if (cmd == "convert")       return cmd_convert(argc, argv);
        if (cmd == "info")          return cmd_info(argc, argv);
        if (cmd == "cache-prepare") return cmd_cache_prepare(argc, argv);
        if (cmd == "bench")         return cmd_bench(argc, argv);
        if (cmd == "verify")   return cmd_verify(argc, argv);
        if (cmd == "extract")  return cmd_extract(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 2;
    }

    std::cerr << "Unknown command: " << cmd << "\n";
    print_usage(argv[0]);
    return 1;
}
