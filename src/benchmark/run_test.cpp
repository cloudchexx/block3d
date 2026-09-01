#include "block3d/reader.hpp"
#include "block3d/converter.hpp"
#include "block3d/rng.hpp"
#include "benchmark_cache/benchmark_cache.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <streambuf>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <exception>
#include <cstdio>
#include <limits>
#include <array>
#include <cstdint>
#include <system_error>

#ifdef _WIN32
#include <io.h>      // _commit
#include <process.h> // _getpid
#else
#include <unistd.h>  // fsync, getpid
#endif

namespace fs = std::filesystem;
using namespace block3d;

// ── Tee streambuf (duplicate cout to log file) ───────────────────────

static std::streambuf* g_console_rdbuf = nullptr;
static std::streambuf* g_console_err_rdbuf = nullptr;
static std::ofstream   g_log_file;
static std::streambuf* g_tee_buf = nullptr;
static std::streambuf* g_err_tee_buf = nullptr;
static std::string     g_run_id;  // shared, unique: log name + per-run output dir

class TeeBuf : public std::streambuf {
    std::streambuf* out1_;
    std::streambuf* out2_;
public:
    TeeBuf(std::streambuf* o1, std::streambuf* o2) : out1_(o1), out2_(o2) {}
protected:
    int overflow(int c) override {
        if (c == EOF) return EOF;
        int r1 = out1_->sputc(static_cast<char>(c));
        out2_->sputc(static_cast<char>(c));
        return r1;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::streamsize r = out1_->sputn(s, n);
        out2_->sputn(s, n);
        return r;
    }
    int sync() override {
        int r = out1_->pubsync();
        out2_->pubsync();
        return r;
    }
};

// ── Cross-platform process id ────────────────────────────────────────
static unsigned long get_pid() {
#ifdef _WIN32
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

// Generate a per-run id with at least millisecond resolution + process id,
// so collisions across rapid re-invocations are avoided.
static std::string make_run_id() {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tm, &tt);
#endif
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    std::ostringstream id;
    id << std::put_time(&tm, "%Y%m%d_%H%M%S")
       << "_" << std::setfill('0') << std::setw(3) << ms.count()
       << "_pid" << get_pid();
    return id.str();
}

static void setup_logging(const std::vector<std::string>& datasets) {
    fs::create_directories("logs");

    std::ostringstream oss;
    oss << "logs/block3d_";
    for (size_t i = 0; i < datasets.size(); i++) {
        if (i > 0) oss << "_";
        oss << datasets[i];
    }
    oss << "_" << g_run_id << ".log";
    std::string log_path = oss.str();

    g_log_file.open(log_path);
    if (!g_log_file) {
        std::cerr << "Warning: cannot create log file: " << log_path << "\n";
        return;
    }

    g_console_rdbuf = std::cout.rdbuf();
    g_console_err_rdbuf = std::cerr.rdbuf();
    g_tee_buf = new TeeBuf(g_console_rdbuf, g_log_file.rdbuf());
    g_err_tee_buf = new TeeBuf(g_console_err_rdbuf, g_log_file.rdbuf());
    std::cout.rdbuf(g_tee_buf);
    std::cerr.rdbuf(g_err_tee_buf);
    std::cout << "Log saved to: " << log_path << "\n\n";
}

static void teardown_logging() {
    if (g_err_tee_buf) {
        std::cerr.rdbuf(g_console_err_rdbuf);
        delete g_err_tee_buf;
        g_err_tee_buf = nullptr;
    }
    if (g_tee_buf) {
        std::cout.rdbuf(g_console_rdbuf);
        delete g_tee_buf;
        g_tee_buf = nullptr;
    }
    if (g_log_file.is_open()) g_log_file.close();
}

// ── Output persistence: flush + push to device ──────────────────────
// After fflush, request the OS to push buffered data to the storage stack.
// NOTE: this does NOT guarantee bypassing the device firmware write cache;
// a true durable barrier would require an explicit ATA/SCSI flush command
// at the device level, which this tool does not issue.
static bool sync_output(FILE* f) {
    if (std::fflush(f) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return ::fsync(::fileno(f)) == 0;
#endif
}

// ── Dataset definitions ──────────────────────────────────────────────

struct DatasetInfo {
    std::string raw;
    std::string b3d;
    uint64_t dim_x, dim_y, dim_z;
};

static std::map<std::string, DatasetInfo> DATASETS = {
    {"test18", {"test18.dat", "test18.b3d", 801, 2405, 2501}},
    {"test50", {"test50.dat", "test50.b3d", 2002, 2202, 3001}},
};

// ── Helpers ──────────────────────────────────────────────────────────

static bool is_valid_b3d(const std::string& path,
                          uint64_t dx, uint64_t dy, uint64_t dz,
                          uint64_t bs = 0) {
    if (!fs::exists(path)) return false;
    try {
        BlockedFileReader reader(path);
        return reader.dim_x() == dx && reader.dim_y() == dy && reader.dim_z() == dz &&
               (bs == 0 || reader.block_size() == bs);
    } catch (...) {
        return false;
    }
}

static std::string size_str(uint64_t bytes) {
    double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << gb << " GB";
    return oss.str();
}

static std::string normalize_dispatch(const std::string& value) {
    return value == "round-robin" ? "round_robin" : value;
}

static ReadDispatchStrategy parse_dispatch_strategy(const std::string& value) {
    return value == "contiguous"
        ? ReadDispatchStrategy::Contiguous
        : ReadDispatchStrategy::RoundRobin;
}

// Resolve data file path, searching multiple directories.
// Search order: CWD, CWD/.., exe_dir, exe_dir/.., exe_dir/../..
static std::string resolve_path(const std::string& name,
                                  const fs::path& cwd,
                                  const fs::path& exe_dir) {
    auto check = [&](const fs::path& base) -> std::string {
        auto p = base / name;
        try {
            if (fs::exists(p)) return fs::absolute(p).string();
        } catch (...) {}
        return "";
    };

    // Search in order
    for (auto& d : {cwd, cwd.parent_path(), exe_dir,
                    exe_dir.parent_path(),
                    exe_dir.parent_path().parent_path()}) {
        auto r = check(d);
        if (!r.empty()) return r;
    }
    return name; // fallback: let caller handle
}

// ── Index generation ─────────────────────────────────────────────────

// Fixed-seed random indices (deterministic across runs).
static std::vector<uint64_t> make_rand_indices(uint32_t seed,
                                                int count,
                                                uint64_t dim) {
    XorShift32 rng(seed);
    std::vector<uint64_t> indices;
    indices.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; i++)
        indices.push_back(rng.rand_u64_mod(dim));
    return indices;
}

// Truly contiguous indices: start, start+1, ... (clamped to dim).
static std::vector<uint64_t> make_seq_indices(uint64_t start,
                                               int count,
                                               uint64_t dim) {
    std::vector<uint64_t> indices;
    if (start >= dim || count <= 0) return indices;
    uint64_t avail = dim - start;
    uint64_t n = std::min<uint64_t>(static_cast<uint64_t>(count), avail);
    indices.reserve(n);
    for (uint64_t i = 0; i < n; i++) indices.push_back(start + i);
    return indices;
}

// ── Steady-state batch benchmark ──────────────────────────────────────
// Reader construction (open + mmap + header parse) is OUTSIDE the timing
// window. The timed region is the steady-state batch: slice reads via
// read_slices_batch + per-slice raw float32 file create/write/close[/sync].
// This is NOT an end-to-end measurement including process/reader setup.

struct BenchResult {
    double read_sec = 0;
    double write_sec = 0;
    double total_sec = 0;
    double avg_ms = 0;
    double throughput = 0;
    double producer_wall_sec = 0;
    double writer_service_sec = 0;
    double drain_sec = 0;
    double producer_wait_sec = 0;
    size_t queue_peak_slices = 0;
    uint64_t queue_peak_bytes = 0;
    uint64_t pipeline_memory_mb = 0;
    size_t pipeline_window_slices = 0;
    size_t pipeline_queue_slices = 0;
    size_t pipeline_max_live_slices = 0;
    uint64_t pipeline_payload_peak_bound_bytes = 0;
    size_t reader_pool_workers = 0;
    uint64_t reader_pool_jobs = 0;
    uint64_t reader_pool_serial_fallbacks = 0;
    size_t   n_read = 0;       // slices returned by read_slices_batch
    size_t   n_written = 0;    // slices fully written (open/write/sync/close ok)
    uint64_t total_bytes = 0;
    std::vector<uint64_t> indices;
    std::vector<uint64_t> slice_bytes;
    bool io_error = false;
    bool pipeline_enabled = false;
};

struct PipelineConfig {
    bool enabled = false;
    uint64_t memory_mb = 256;
    size_t window_slices = 0; // 0 = auto
};

struct PipelineRuntimeConfig {
    size_t read_window_slices = 1;
    size_t queue_capacity_slices = 1;
    size_t max_live_slices = 1;
    uint64_t memory_bytes = 0;
};

static fs::path output_file_path(const fs::path& out_dir,
                                const std::string& ds_name,
                                char axis,
                                const std::string& mode,
                                size_t seq,
                                uint64_t index) {
    std::ostringstream fn;
    fn << ds_name << "_" << axis << "_" << mode << "_"
       << std::setw(4) << std::setfill('0') << seq
       << "_idx" << index << ".raw";
    return out_dir / fn.str();
}

struct SliceJob {
    size_t request_pos = 0;
    uint64_t index = 0;
    fs::path output_path;
    std::vector<float> data;
};

struct WriteResult {
    bool ok = false;
    uint64_t bytes = 0;
    std::string error_path;
};

static WriteResult write_slice_file(const SliceJob& job, bool output_sync) {
    WriteResult result;
    result.error_path = job.output_path.string();
    std::string fp_str = job.output_path.string();

    FILE* f = std::fopen(fp_str.c_str(), "wb");
    if (!f) return result;

    result.bytes = job.data.size() * sizeof(float);
    bool ok = std::fwrite(job.data.data(), 1,
                          static_cast<size_t>(result.bytes), f)
              == static_cast<size_t>(result.bytes);
    if (ok && output_sync) ok = sync_output(f);
    if (std::fclose(f) != 0) ok = false;
    result.ok = ok;
    return result;
}

class BoundedSliceQueue {
public:
    explicit BoundedSliceQueue(size_t capacity_slices)
        : capacity_slices_(std::max<size_t>(1, capacity_slices)) {}

    bool push(SliceJob&& job, double& wait_sec) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto wait_begin = std::chrono::high_resolution_clock::now();
        not_full_.wait(lock, [&] { return cancelled_ || queue_.size() < capacity_slices_; });
        auto wait_end = std::chrono::high_resolution_clock::now();
        wait_sec += std::chrono::duration<double>(wait_end - wait_begin).count();
        if (cancelled_) return false;

        uint64_t bytes = job.data.size() * sizeof(float);
        queued_bytes_ += bytes;
        queue_.push_back(std::move(job));
        peak_slices_ = std::max(peak_slices_, queue_.size());
        peak_bytes_ = std::max(peak_bytes_, queued_bytes_);
        not_empty_.notify_one();
        return true;
    }

    bool pop(SliceJob& job) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [&] { return closed_ || cancelled_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        job = std::move(queue_.front());
        queue_.pop_front();
        queued_bytes_ -= job.data.size() * sizeof(float);
        not_full_.notify_one();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
    }

    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
        queue_.clear();
        queued_bytes_ = 0;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    size_t peak_slices() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return peak_slices_;
    }

    uint64_t peak_bytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return peak_bytes_;
    }

private:
    size_t capacity_slices_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<SliceJob> queue_;
    uint64_t queued_bytes_ = 0;
    size_t peak_slices_ = 0;
    uint64_t peak_bytes_ = 0;
    bool closed_ = false;
    bool cancelled_ = false;
};

static uint64_t mb_to_bytes(uint64_t mb) {
    if (mb > std::numeric_limits<uint64_t>::max() / (1024ULL * 1024ULL))
        return std::numeric_limits<uint64_t>::max();
    return mb * 1024ULL * 1024ULL;
}

static PipelineRuntimeConfig make_pipeline_runtime(uint64_t slice_bytes,
                                                   const PipelineConfig& config) {
    PipelineRuntimeConfig runtime;
    runtime.memory_bytes = mb_to_bytes(config.memory_mb);
    uint64_t max_live = runtime.memory_bytes / std::max<uint64_t>(1, slice_bytes);
    if (max_live == 0) max_live = 1;
    runtime.max_live_slices = static_cast<size_t>(std::min<uint64_t>(
        max_live, static_cast<uint64_t>(std::numeric_limits<size_t>::max())));

    uint64_t window_limit = max_live > 1 ? max_live - 1 : 1;
    if (config.window_slices > 0) {
        runtime.read_window_slices = static_cast<size_t>(std::min<uint64_t>(
            static_cast<uint64_t>(config.window_slices), window_limit));
    } else {
        runtime.read_window_slices = static_cast<size_t>(std::min<uint64_t>(4, window_limit));
    }
    runtime.read_window_slices = std::max<size_t>(1, runtime.read_window_slices);

    if (max_live > 1) {
        runtime.queue_capacity_slices = static_cast<size_t>(std::max<uint64_t>(
            1, max_live - static_cast<uint64_t>(runtime.read_window_slices)));
    } else {
        runtime.queue_capacity_slices = 0;
    }
    return runtime;
}

static BenchResult run_bench(const std::string& b3d_path,
                              char axis,
                              const std::string& mode,
                              const std::vector<uint64_t>& indices,
                              const fs::path& out_dir,
                              const std::string& ds_name,
                              bool output_sync,
                              const std::string& batch_read,
                              size_t batch_window_slices,
                              const PipelineConfig& pipeline_config,
                              uint64_t planned_slice_bytes,
                              ReadDispatchStrategy dispatch_strategy) {
    // Reader construction (mmap + header parse) is intentionally OUTSIDE the
    // timing window: this is a steady-state batch measurement, not an
    // end-to-end measurement that includes reader/process setup.
    BlockedFileReader reader(b3d_path, 0, 0, dispatch_strategy);
    BenchResult r;
    r.indices = indices;
    r.slice_bytes.assign(indices.size(), 0);
    r.pipeline_enabled = pipeline_config.enabled;

    auto t0 = std::chrono::high_resolution_clock::now();
    if (!pipeline_config.enabled) {
        std::vector<std::vector<float>> slices;
        if (batch_read == "legacy") {
            slices.reserve(indices.size());
            for (uint64_t index : indices) slices.push_back(reader.read_slice(axis, index));
        } else {
            SliceBatchOptions options;
            options.window_slices = batch_window_slices;
            slices.resize(indices.size());
            reader.read_slices_batch_stream(axis, indices, options,
                [&](size_t request_pos, uint64_t, std::vector<float>&& slice) {
                    slices[request_pos] = std::move(slice);
                });
        }
        auto t_read_done = std::chrono::high_resolution_clock::now();
        r.read_sec = std::chrono::duration<double>(t_read_done - t0).count();
        r.producer_wall_sec = r.read_sec;
        r.n_read = slices.size();

        // A read-count mismatch is itself an I/O failure: do not write partial
        // output and do not emit success metrics for this case.
        if (slices.size() != indices.size()) {
            std::cerr << "[ERROR] read_slices_batch returned " << slices.size()
                      << " slices for " << indices.size()
                      << " requested (axis=" << axis << ", mode=" << mode << ")\n";
            r.io_error = true;
            auto t1 = std::chrono::high_resolution_clock::now();
            r.total_sec = std::chrono::duration<double>(t1 - t0).count();
            r.write_sec = std::chrono::duration<double>(t1 - t_read_done).count();
            return r;
        }

        auto t_write0 = std::chrono::high_resolution_clock::now();
        for (size_t i = 0; i < slices.size(); i++) {
            SliceJob job;
            job.request_pos = i;
            job.index = indices[i];
            job.output_path = output_file_path(out_dir, ds_name, axis, mode, i, indices[i]);
            job.data = std::move(slices[i]);

            WriteResult wr = write_slice_file(job, output_sync);
            if (!wr.ok) {
                std::cerr << "[ERROR] write/sync/close failed for: " << job.output_path << "\n";
                r.io_error = true;
                continue;
            }
            r.slice_bytes[i] = wr.bytes;
            r.total_bytes += wr.bytes;
            r.n_written++;
        }
        auto t1 = std::chrono::high_resolution_clock::now();

        r.write_sec = std::chrono::duration<double>(t1 - t_write0).count();
        r.writer_service_sec = r.write_sec;
        r.total_sec = std::chrono::duration<double>(t1 - t0).count();
    } else {
        PipelineRuntimeConfig runtime = make_pipeline_runtime(planned_slice_bytes, pipeline_config);
        r.pipeline_memory_mb = pipeline_config.memory_mb;
        r.pipeline_window_slices = runtime.read_window_slices;
        r.pipeline_queue_slices = runtime.queue_capacity_slices;
        r.pipeline_max_live_slices = runtime.max_live_slices;
        r.pipeline_payload_peak_bound_bytes = static_cast<uint64_t>(runtime.max_live_slices) * planned_slice_bytes;
        auto producer_done = t0;
        std::exception_ptr producer_exception;
        std::string first_error_path;

        auto record_write = [&](const SliceJob& job, const WriteResult& wr, double service_sec) {
            r.writer_service_sec += service_sec;
            if (!wr.ok) {
                if (first_error_path.empty()) first_error_path = wr.error_path;
                r.io_error = true;
                return;
            }
            if (job.request_pos < r.slice_bytes.size()) {
                r.slice_bytes[job.request_pos] = wr.bytes;
                r.total_bytes += wr.bytes;
                r.n_written++;
            } else {
                if (first_error_path.empty()) first_error_path = job.output_path.string();
                r.io_error = true;
            }
        };

        if (runtime.queue_capacity_slices == 0) {
            try {
                auto consume_inline = [&](size_t request_pos, uint64_t index, std::vector<float>&& slice) {
                    if (r.io_error) return;
                    SliceJob job;
                    job.request_pos = request_pos;
                    job.index = index;
                    job.output_path = output_file_path(out_dir, ds_name, axis, mode,
                                                       request_pos, index);
                    job.data = std::move(slice);
                    r.n_read++;
                    auto w0 = std::chrono::high_resolution_clock::now();
                    WriteResult wr = write_slice_file(job, output_sync);
                    auto w1 = std::chrono::high_resolution_clock::now();
                    record_write(job, wr, std::chrono::duration<double>(w1 - w0).count());
                };

                if (batch_read == "legacy") {
                    for (size_t i = 0; i < indices.size(); i++) {
                        if (r.io_error) break;
                        consume_inline(i, indices[i], reader.read_slice(axis, indices[i]));
                    }
                } else {
                    SliceBatchOptions options;
                    options.window_slices = runtime.read_window_slices;
                    reader.read_slices_batch_stream(axis, indices, options, consume_inline);
                }
                producer_done = std::chrono::high_resolution_clock::now();
            } catch (...) {
                producer_exception = std::current_exception();
                producer_done = std::chrono::high_resolution_clock::now();
            }
        } else {
            BoundedSliceQueue queue(runtime.queue_capacity_slices);
            std::atomic<bool> writer_failed{false};
            std::mutex result_mutex;

            auto writer = std::thread([&] {
                SliceJob job;
                while (queue.pop(job)) {
                    auto w0 = std::chrono::high_resolution_clock::now();
                    WriteResult wr = write_slice_file(job, output_sync);
                    auto w1 = std::chrono::high_resolution_clock::now();

                    std::lock_guard<std::mutex> lock(result_mutex);
                    record_write(job, wr, std::chrono::duration<double>(w1 - w0).count());
                    if (!wr.ok || job.request_pos >= r.slice_bytes.size()) {
                        writer_failed.store(true);
                        queue.cancel();
                    }
                }
            });

            try {
                if (batch_read == "legacy") {
                    for (size_t i = 0; i < indices.size(); i++) {
                        if (writer_failed.load()) break;
                        SliceJob job;
                        job.request_pos = i;
                        job.index = indices[i];
                        job.output_path = output_file_path(out_dir, ds_name, axis, mode, i, indices[i]);
                        job.data = reader.read_slice(axis, indices[i]);
                        r.n_read++;
                        double wait_sec = 0;
                        if (!queue.push(std::move(job), wait_sec)) break;
                        r.producer_wait_sec += wait_sec;
                    }
                } else {
                    SliceBatchOptions options;
                    options.window_slices = runtime.read_window_slices;
                    reader.read_slices_batch_stream(axis, indices, options,
                        [&](size_t request_pos, uint64_t index, std::vector<float>&& slice) {
                            if (writer_failed.load()) return;
                            SliceJob job;
                            job.request_pos = request_pos;
                            job.index = index;
                            job.output_path = output_file_path(out_dir, ds_name, axis, mode,
                                                               request_pos, index);
                            job.data = std::move(slice);
                            r.n_read++;
                            double wait_sec = 0;
                            if (!queue.push(std::move(job), wait_sec)) return;
                            r.producer_wait_sec += wait_sec;
                        });
                }
                producer_done = std::chrono::high_resolution_clock::now();
                queue.close();
            } catch (...) {
                producer_exception = std::current_exception();
                producer_done = std::chrono::high_resolution_clock::now();
                queue.cancel();
            }

            writer.join();
            r.queue_peak_slices = queue.peak_slices();
            r.queue_peak_bytes = queue.peak_bytes();
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        r.total_sec = std::chrono::duration<double>(t1 - t0).count();
        r.producer_wall_sec = std::chrono::duration<double>(producer_done - t0).count();
        r.drain_sec = std::chrono::duration<double>(t1 - producer_done).count();
        r.read_sec = r.producer_wall_sec;
        r.write_sec = r.writer_service_sec;

        if (producer_exception) {
            r.io_error = true;
            try {
                std::rethrow_exception(producer_exception);
            } catch (const std::exception& e) {
                std::cerr << "[ERROR] producer failed: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[ERROR] producer failed with unknown exception\n";
            }
        }
        if (r.io_error && !first_error_path.empty()) {
            std::cerr << "[ERROR] write/sync/close failed for: " << first_error_path << "\n";
        }
    }

    r.reader_pool_workers = reader.thread_pool_workers();
    r.reader_pool_jobs = reader.thread_pool_jobs();
    r.reader_pool_serial_fallbacks = reader.thread_pool_serial_fallbacks();

    if (r.n_read != indices.size() || r.n_written != indices.size()) {
        r.io_error = true;
        std::cerr << "[ERROR] incomplete case axis=" << axis << " mode=" << mode
                  << " requested=" << indices.size()
                  << " read=" << r.n_read
                  << " written=" << r.n_written << "\n";
    }

    // Success metrics exist ONLY when every requested slice was written.
    if (!r.io_error && r.n_written > 0 && r.n_written == r.n_read
        && r.total_sec > 0) {
        r.avg_ms     = r.total_sec / static_cast<double>(r.n_written) * 1000.0;
        r.throughput = static_cast<double>(r.n_written) / r.total_sec;
    }
    return r;
}

static void print_sep(const std::string& title) {
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "  " << title << "\n";
    std::cout << std::string(70, '=') << "\n";
}

static void log_pipeline_result(const BenchResult& r) {
    if (!r.pipeline_enabled) return;
    std::cout << "TIMING_MODEL value=overlapped_pipeline\n";
    std::cout << "PIPELINE_RESULT producer_wall_sec=" << r.producer_wall_sec
              << " writer_service_sec=" << r.writer_service_sec
              << " drain_sec=" << r.drain_sec
              << " producer_wait_sec=" << r.producer_wait_sec
              << " queue_peak_slices=" << r.queue_peak_slices
              << " queue_peak_bytes=" << r.queue_peak_bytes
              << " payload_peak_bound_bytes=" << r.pipeline_payload_peak_bound_bytes
              << " memory_mb=" << r.pipeline_memory_mb
              << " window_slices=" << r.pipeline_window_slices
              << " queue_slices=" << r.pipeline_queue_slices
              << " max_live_slices=" << r.pipeline_max_live_slices
              << "\n";
}

static void log_per_slice_bytes(const BenchResult& r) {
    if (r.slice_bytes.empty()) {
        std::cout << "         per-slice bytes: (none)\n";
        return;
    }
    bool uniform = true;
    for (size_t i = 1; i < r.slice_bytes.size(); i++)
        if (r.slice_bytes[i] != r.slice_bytes[0]) { uniform = false; break; }
    std::cout << "         per-slice bytes: ";
    if (uniform) {
        std::cout << r.slice_bytes[0] << " x" << r.slice_bytes.size()
                  << " (uniform), total=" << r.total_bytes << "\n";
    } else {
        std::cout << "[";
        for (size_t i = 0; i < r.slice_bytes.size(); i++) {
            if (i) std::cout << ",";
            std::cout << r.slice_bytes[i];
        }
        std::cout << "], total=" << r.total_bytes << "\n";
    }
}

// Print a bench table only for fully-successful cases. The caller must NOT
// invoke this when any case in the group had an I/O error (we abort instead).
static void print_bench_table(const std::string& title,
                               const std::vector<char>& axes,
                               const std::vector<BenchResult>& results,
                               const std::string& cache_name,
                               const std::string& mode,
                               const std::vector<uint64_t>& plan_hashes,
                               bool output_sync) {
    std::cout << "\n[" << title << "]\n";
    std::cout << "         " << std::left << std::setw(6) << "Axis"
              << std::right << std::setw(10) << "Total"
              << std::setw(12) << "Avg"
              << std::setw(14) << "Throughput" << "\n";
    std::cout << "         " << std::string(6, '-') << " "
              << std::string(8, '-') << " "
              << std::string(10, '-') << " "
              << std::string(12, '-') << "\n";

    double sum = 0, mn = std::numeric_limits<double>::max(), mx = 0;
    int cnt = 0;
    for (size_t i = 0; i < axes.size(); i++) {
        const auto& r = results[i];
        std::cout << "         "
                  << std::left << std::setw(6)
                  << static_cast<char>(std::toupper(axes[i]))
                  << std::right << std::fixed << std::setprecision(3)
                  << std::setw(9) << r.total_sec << "s"
                  << std::setw(10) << std::setprecision(2) << r.avg_ms << "ms"
                  << std::setw(12) << std::setprecision(1) << r.throughput
                  << " slices/s  (read_sec=" << std::setprecision(3) << r.read_sec
                  << "s, write_sec=" << r.write_sec << "s, read " << r.n_read
                  << ", written " << r.n_written << ", "
                  << size_str(r.total_bytes) << " written)\n";
        log_per_slice_bytes(r);
        log_pipeline_result(r);
        std::cout << "BENCHMARK_RESULT cache=" << cache_name
                  << " mode=" << mode
                  << " axis=" << axes[i]
                  << " total_sec=" << r.total_sec
                  << " read_sec=" << r.read_sec
                  << " write_sec=" << r.write_sec
                  << " avg_ms=" << r.avg_ms
                  << " slices_per_sec=" << r.throughput
                  << " count=" << r.n_written
                  << " bytes=" << r.total_bytes
                  << " reader_pool_workers=" << r.reader_pool_workers
                  << " reader_pool_jobs=" << r.reader_pool_jobs
                  << " reader_pool_serial_fallbacks=" << r.reader_pool_serial_fallbacks
                  << " output_sync=" << (output_sync ? "requested" : "disabled")
                  << " plan_hash=" << std::hex << std::setw(16) << std::setfill('0')
                  << plan_hashes[i] << std::dec << std::setfill(' ') << "\n";
        if (r.n_written > 0) {
            sum += r.avg_ms;
            if (r.avg_ms < mn) mn = r.avg_ms;
            if (r.avg_ms > mx) mx = r.avg_ms;
            cnt++;
        }
    }

    if (cnt > 0) {
        double avg = sum / static_cast<double>(cnt);
        std::cout << "         " << std::string(42, '-') << "\n";
        std::cout << "         " << std::left << std::setw(6) << "Avg"
                  << std::right << std::setw(12)
                  << std::setprecision(2) << avg << "ms";
        if (cnt > 1 && mn > 0)
            std::cout << "  balance=" << std::setprecision(2) << (mx / mn) << "x";
        else
            std::cout << "  balance=n/a";
        std::cout << "  (over " << cnt
                  << (cnt > 1 ? " axes" : " axis") << ")\n";
    }
}

// ── Full test for one dataset ────────────────────────────────────────

// Tracks whether ANY b3d conversion has happened in this process. If so, the
// freshly-written b3d pages are very likely resident in the OS page cache, so
// benchmark numbers measured later in the SAME process are NOT cold-cache.
// This flag is process-wide (not per-dataset).
static bool g_converted_b3d_in_process = false;

struct BenchCasePlan {
    char axis = 'x';
    std::string mode;
    std::string file_mode;
    std::vector<uint64_t> indices;
    uint64_t plan_hash = 0;
};

struct PhaseResults {
    std::string cache_name;
    std::map<std::string, std::vector<BenchResult>> by_mode;
};

static std::string hex_u64(uint64_t value) {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << value;
    return oss.str();
}

static uint64_t compute_plan_hash(const BenchCasePlan& plan,
                                  const DatasetInfo& cfg,
                                  uint32_t seed,
                                  uint64_t seq_start) {
    uint64_t hash = 1469598103934665603ULL;
    hash = fnv1a64_update(hash, &plan.axis, sizeof(plan.axis));
    hash = fnv1a64_update(hash, plan.mode.data(), plan.mode.size());
    hash = fnv1a64_u64(hash, cfg.dim_x);
    hash = fnv1a64_u64(hash, cfg.dim_y);
    hash = fnv1a64_u64(hash, cfg.dim_z);
    hash = fnv1a64_u64(hash, seed);
    hash = fnv1a64_u64(hash, seq_start);
    for (uint64_t index : plan.indices) hash = fnv1a64_u64(hash, index);
    return hash;
}

static std::vector<BenchCasePlan> build_case_plans(
        const DatasetInfo& cfg,
        int random_count,
        int seq_count,
        uint32_t seed,
        uint64_t seq_start,
        const std::vector<char>& axes,
        const std::vector<std::string>& modes) {
    std::vector<BenchCasePlan> plans;
    for (const auto& mode : modes) {
        bool random = mode == "random";
        for (char axis : axes) {
            uint64_t dim = axis == 'x' ? cfg.dim_x : axis == 'y' ? cfg.dim_y : cfg.dim_z;
            BenchCasePlan plan;
            plan.axis = axis;
            plan.mode = mode;
            plan.file_mode = random ? "rand" : "seq";
            plan.indices = random
                ? make_rand_indices(seed, random_count, dim)
                : make_seq_indices(seq_start, seq_count, dim);
            plan.plan_hash = compute_plan_hash(plan, cfg, seed, seq_start);
            plans.push_back(std::move(plan));
        }
    }
    return plans;
}

static void log_scrub_result(const CacheScrubResult& scrub) {
    std::cout << "CACHE_SCRUB method=unrelated_file_sweep\n"
              << "CACHE_SCRUB file=" << scrub.path << "\n"
              << "CACHE_SCRUB bytes=" << scrub.bytes_read
              << " file_bytes=" << scrub.file_bytes
              << " required_bytes=" << scrub.required_bytes
              << " ram_ratio=" << scrub.ram_ratio
              << " passes=" << scrub.passes << "\n"
              << "CACHE_SCRUB elapsed_sec=" << scrub.elapsed_sec
              << " checksum=" << scrub.checksum << "\n"
              << "CACHE_VALIDITY state=" << scrub.message << "\n";
}

static bool run_cold_prepare(const CacheOptions& cache_options) {
    if (cache_options.cold_method == ColdMethod::None) {
        std::cout << "CACHE_SCRUB method=none\n"
                  << "CACHE_VALIDITY state=cold_first_touch\n";
        return true;
    }
    std::cout << "[Phase] Cache scrub\n";
    auto scrub = run_cache_scrub(cache_options);
    log_scrub_result(scrub);
    if (!scrub.ok) {
        std::cerr << "[ERROR] cache scrub failed: " << scrub.message << "\n";
        return false;
    }
    return true;
}

static void warm_up_for_plans(const std::string& b3d_path,
                              const std::vector<BenchCasePlan>& plans,
                              WarmupScope scope,
                              size_t stride,
                              uint64_t memory_mb,
                              const std::string& batch_read,
                              size_t batch_window_slices,
                              ReadDispatchStrategy dispatch_strategy) {
    auto t0 = std::chrono::high_resolution_clock::now();
    uint64_t bytes = 0;
    uint64_t checksum = 0;
    {
        BlockedFileReader reader(b3d_path, 0, 0, dispatch_strategy);
        if (scope == WarmupScope::Dataset) {
            uint64_t bs = reader.block_size();
            bytes = reader.total_blocks() * bs * bs * bs * sizeof(float);
            reader.warm_up(false, stride, memory_mb);
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
                    options.window_slices = batch_window_slices;
                    slices.resize(plan.indices.size());
                    reader.read_slices_batch_stream(plan.axis, plan.indices, options,
                        [&](size_t request_pos, uint64_t, std::vector<float>&& slice) {
                            slices[request_pos] = std::move(slice);
                        });
                }
                for (const auto& slice : slices) {
                    bytes += slice.size() * sizeof(float);
                    if (!slice.empty()) {
                        checksum ^= static_cast<uint64_t>(slice.front() * 1000003.0f);
                        checksum ^= static_cast<uint64_t>(slice.back() * 9176.0f);
                    }
                }
            }
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    std::cout << "WARMUP_RESULT scope=" << to_string(scope)
              << " elapsed_sec=" << std::chrono::duration<double>(t1 - t0).count()
              << " bytes=" << bytes
              << " checksum=" << checksum << "\n";
}

static bool files_equal(const fs::path& lhs, const fs::path& rhs) {
    std::error_code ec;
    uint64_t lhs_size = fs::file_size(lhs, ec);
    if (ec) return false;
    uint64_t rhs_size = fs::file_size(rhs, ec);
    if (ec || lhs_size != rhs_size) return false;
    std::ifstream a(lhs, std::ios::binary);
    std::ifstream b(rhs, std::ios::binary);
    std::vector<char> abuf(1024 * 1024);
    std::vector<char> bbuf(1024 * 1024);
    while (a && b) {
        a.read(abuf.data(), static_cast<std::streamsize>(abuf.size()));
        b.read(bbuf.data(), static_cast<std::streamsize>(bbuf.size()));
        auto an = a.gcount();
        auto bn = b.gcount();
        if (an != bn || std::memcmp(abuf.data(), bbuf.data(), static_cast<size_t>(an)) != 0)
            return false;
    }
    return a.eof() && b.eof();
}

static uint64_t slice_bytes_for_axis(const DatasetInfo& cfg, char axis) {
    if (axis == 'x') return cfg.dim_y * cfg.dim_z * sizeof(float);
    if (axis == 'y') return cfg.dim_x * cfg.dim_z * sizeof(float);
    return cfg.dim_x * cfg.dim_y * sizeof(float);
}

static uint64_t estimate_output_bytes(const DatasetInfo& cfg,
                                      const std::vector<BenchCasePlan>& plans,
                                      CacheMode mode) {
    uint64_t per_phase = 0;
    for (const auto& plan : plans) {
        uint64_t one = slice_bytes_for_axis(cfg, plan.axis);
        if (plan.indices.size() > std::numeric_limits<uint64_t>::max() / one)
            return std::numeric_limits<uint64_t>::max();
        uint64_t add = one * static_cast<uint64_t>(plan.indices.size());
        if (per_phase > std::numeric_limits<uint64_t>::max() - add)
            return std::numeric_limits<uint64_t>::max();
        per_phase += add;
    }
    uint64_t phases = cache_mode_has_cold(mode) && cache_mode_has_hot(mode) ? 2 : 1;
    if (per_phase > std::numeric_limits<uint64_t>::max() / phases)
        return std::numeric_limits<uint64_t>::max();
    return per_phase * phases;
}

static bool verify_phase_outputs(const fs::path& cold_dir,
                                 const fs::path& hot_dir,
                                 const std::string& ds_name,
                                 const std::vector<BenchCasePlan>& plans) {
    uint64_t files = 0;
    uint64_t bytes = 0;
    for (const auto& plan : plans) {
        for (size_t i = 0; i < plan.indices.size(); ++i) {
            auto cold = output_file_path(cold_dir, ds_name, plan.axis,
                                         plan.file_mode, i, plan.indices[i]);
            auto hot = output_file_path(hot_dir, ds_name, plan.axis,
                                        plan.file_mode, i, plan.indices[i]);
            if (!files_equal(cold, hot)) {
                std::cerr << "[ERROR] phase output mismatch: " << cold
                          << " vs " << hot << "\n";
                std::cout << "OUTPUT_VERIFY_RESULT ok=0 files_checked=" << files
                          << " mismatch=" << cold.filename().string() << "\n";
                return false;
            }
            bytes += fs::file_size(cold);
            files++;
        }
    }
    std::cout << "OUTPUT_VERIFY_RESULT ok=1 files_checked=" << files
              << " bytes_checked=" << bytes << "\n";
    return true;
}

static int run_full_test(const std::string& ds_name,
                           const DatasetInfo& cfg,
                           uint64_t block_size,
                           int random_count, int seq_count,
                           bool verify_enabled, int verify_samples,
                           const CacheOptions& cache_options,
                           size_t warmup_stride,
                           uint64_t warmup_memory_mb,
                           uint32_t seed,
                           uint64_t seq_start,
                           const fs::path& run_dir,
                           const std::vector<char>& sel_axes,
                           const std::vector<std::string>& sel_modes,
                           bool output_sync,
                           const std::string& batch_read,
                           size_t batch_window_slices,
                           const PipelineConfig& pipeline_config,
                           ReadDispatchStrategy dispatch_strategy,
                           bool explicit_b3d_input) {
    print_sep("Dataset: " + ds_name + "  (" +
              std::to_string(cfg.dim_x) + "x" +
              std::to_string(cfg.dim_y) + "x" +
              std::to_string(cfg.dim_z) + ")");

    std::string b3d_path = cfg.b3d;
    std::string raw_path = cfg.raw;

    // raw is only required when conversion or explicit verification needs it.
    if (!explicit_b3d_input && !fs::exists(raw_path)) {
        std::cout << "[ERROR] raw file '" << raw_path << "' not found for "
                  << ds_name << "\n";
        return 1;
    }

    // Auto-detect block_size when not explicitly set and conversion may be needed.
    if (block_size == 0 && !explicit_b3d_input) {
        auto slash = b3d_path.find_last_of("/\\");
        std::string parent =
            (slash != std::string::npos) ? b3d_path.substr(0, slash) : ".";
        auto sc = detect_storage_medium(parent);
        block_size = auto_block_size(cfg.dim_x, cfg.dim_y, cfg.dim_z, sc);
        const char* sc_names[] = {"HDD", "SSD", "NVMe", "Unknown"};
        std::cout << "[AUTO-BS] Detected "
                  << sc_names[static_cast<int>(sc)]
                  << " storage, auto block_size=" << block_size << "\n";
    }

    // 1. Convert (skip if valid)
    if (is_valid_b3d(b3d_path, cfg.dim_x, cfg.dim_y, cfg.dim_z, block_size)) {
        auto sz = fs::file_size(b3d_path);
        std::cout << "[SKIP] " << ds_name << ": '" << b3d_path
                  << "' already exists (" << size_str(sz)
                  << "), valid format\n";
    } else if (explicit_b3d_input) {
        std::cout << "[ERROR] explicit --b3d-file is not a valid .b3d for "
                  << ds_name << " or does not match requested dimensions/block size: "
                  << b3d_path << "\n";
        return 1;
    } else {
        std::cout << "[CONVERT] " << ds_name << ": " << raw_path
                  << " -> " << b3d_path << "\n";
        std::cout << "         dims=" << cfg.dim_x << "x" << cfg.dim_y
                  << "x" << cfg.dim_z << ", block_size=" << block_size
                  << ", threads=auto\n";
        // Record that this process converted a b3d. The freshly-written b3d
        // pages are very likely resident in the OS page cache, so any
        // benchmark measured later in THIS process is NOT cold-cache.
        g_converted_b3d_in_process = true;

        auto t0 = std::chrono::high_resolution_clock::now();
        convert_raw_to_blocked(raw_path, b3d_path,
                               cfg.dim_x, cfg.dim_y, cfg.dim_z,
                               block_size, 0, true);
        auto t1 = std::chrono::high_resolution_clock::now();
        double dt = std::chrono::duration<double>(t1 - t0).count();
        auto b3d_sz = fs::file_size(b3d_path);
        auto raw_sz = fs::file_size(raw_path);
        double ratio = static_cast<double>(b3d_sz) / static_cast<double>(raw_sz);
        std::cout << "         done in " << std::fixed
                  << std::setprecision(1) << dt << "s, "
                  << size_str(b3d_sz) << ", storage ratio "
                  << std::setprecision(3) << ratio << "x\n";
    }

    {
        BlockedFileReader metadata_reader(b3d_path);
        const char* layout_name = metadata_reader.inner_layout() == BlockInnerLayout::MicroTiledXYZ
            ? "micro-tiled"
            : "legacy";
        std::cout << "B3D_FORMAT dataset=" << ds_name
                  << " file=" << b3d_path
                  << " version=" << metadata_reader.version()
                  << " layout=" << layout_name
                  << " micro_size=" << metadata_reader.micro_size()
                  << " block_size=" << metadata_reader.block_size()
                  << "\n";
    }

    // 2. Verify (opt-in). Reads the raw input, so it pollutes the page cache
    //    for THIS process; subsequent benchmarks are NOT cold-cache results.
    if (verify_enabled) {
        if (!fs::exists(raw_path)) {
            std::cout << "[ERROR] --verify requires raw file '" << raw_path
                      << "' for " << ds_name << "\n";
            return 1;
        }
        if (verify_samples <= 0) {
            std::cout << "[ERROR] --verify requested but verify-samples="
                      << verify_samples << " is not positive\n";
            return 1;
        }
        std::cout << "\n[VERIFY] *** --verify ENABLED for this process ***\n"
                  << "         Verifying " << verify_samples
                  << " random points by reading the RAW input file.\n"
                  << "         This populates the OS page cache; benchmark "
                     "numbers in THIS process\n"
                  << "         are NOT cold-cache results. Use --verify for "
                     "correctness checks only.\n";
        BlockedFileReader reader(b3d_path);
        auto t0 = std::chrono::high_resolution_clock::now();
        bool ok = reader.verify(raw_path, static_cast<uint64_t>(verify_samples));
        auto t1 = std::chrono::high_resolution_clock::now();
        double dt = std::chrono::duration<double>(t1 - t0).count();
        if (ok) {
            std::cout << "         PASSED in " << std::fixed
                      << std::setprecision(2) << dt << "s\n";
        } else {
            std::cout << "         FAILED! Data mismatch detected.\n";
            return 1;
        }
    }

    unsigned hc = std::thread::hardware_concurrency();
    int thread_count = static_cast<int>(hc > 0 ? hc : 1);
    auto plans = build_case_plans(cfg, random_count, seq_count, seed,
                                  seq_start, sel_axes, sel_modes);
    if (plans.empty()) {
        std::cerr << "[ERROR] no benchmark cases generated\n";
        return 1;
    }
    for (const auto& plan : plans) {
        if (plan.indices.empty()) {
            std::cerr << "[ERROR] generated " << plan.mode << " case for axis '"
                      << plan.axis << "' produced 0 requests\n";
            return 1;
        }
    }

    uint64_t suite_hash = 1469598103934665603ULL;
    for (const auto& plan : plans) suite_hash = fnv1a64_u64(suite_hash, plan.plan_hash);

    std::cout << "\n[CONFIG] seed=" << seed << " (fixed for random reads)\n"
              << "         threads=" << thread_count << " (auto)\n"
              << "         run_dir=" << run_dir.string() << "\n"
              << "         output_pattern=<phase>/" << ds_name
              << "_<axis>_<mode>_<seq>_<idx>.raw\n"
              << "         output_sync=" << (output_sync ? "requested" : "disabled") << "\n"
              << "         timing_scope=read_write_total\n";
    std::cout << "CACHE_CONFIG mode=" << to_string(cache_options.mode)
              << " cold_method=" << to_string(cache_options.cold_method)
              << " cold_isolation=" << to_string(cache_options.cold_isolation)
              << " warmup_scope=" << to_string(cache_options.warmup_scope)
              << " timing_scope=read_write_total\n"
              << "TIMING_SCOPE value=read_write_total\n"
              << "PLAN_HASH_SUITE value=" << hex_u64(suite_hash) << "\n";
    for (const auto& plan : plans) {
        std::cout << "PLAN_HASH axis=" << plan.axis
                  << " mode=" << plan.mode
                  << " value=" << hex_u64(plan.plan_hash)
                  << " count=" << plan.indices.size() << "\n";
    }

    uint64_t estimated_bytes = estimate_output_bytes(cfg, plans, cache_options.mode);
    auto available_bytes = fs::space(run_dir).available;
    std::cout << "OUTPUT_SPACE estimated_bytes=" << estimated_bytes
              << " available_bytes=" << available_bytes << "\n";
    if (estimated_bytes > available_bytes) {
        std::cerr << "[ERROR] insufficient output space for benchmark outputs\n";
        return 1;
    }

    fs::path cold_dir = run_dir / (cache_options.cold_method == ColdMethod::Scrub
                                   ? "cold_scrubbed" : "cold_first_touch");
    fs::path hot_dir = run_dir / "hot_prefetched";
    std::error_code ec;
    if (cache_mode_has_cold(cache_options.mode)) fs::create_directories(cold_dir, ec);
    if (!ec && cache_mode_has_hot(cache_options.mode)) fs::create_directories(hot_dir, ec);
    if (ec) {
        std::cerr << "[ERROR] cannot create phase output directories: " << ec.message() << "\n";
        return 1;
    }

    auto run_phase = [&](const std::string& cache_name,
                         const fs::path& phase_dir,
                         bool scrub_each_case,
                         PhaseResults& phase_results) -> bool {
        phase_results.cache_name = cache_name;
        for (const auto& mode : sel_modes) {
            std::vector<BenchResult> results;
            std::vector<uint64_t> hashes;
            results.reserve(sel_axes.size());
            hashes.reserve(sel_axes.size());
            for (char axis : sel_axes) {
                auto it = std::find_if(plans.begin(), plans.end(), [&](const BenchCasePlan& plan) {
                    return plan.mode == mode && plan.axis == axis;
                });
                if (it == plans.end()) return false;
                if (scrub_each_case && !run_cold_prepare(cache_options)) return false;
                auto result = run_bench(b3d_path, it->axis, it->file_mode,
                                        it->indices, phase_dir, ds_name, output_sync,
                                        batch_read, batch_window_slices,
                                        pipeline_config,
                                        slice_bytes_for_axis(cfg, it->axis),
                                        dispatch_strategy);
                if (result.io_error) return false;
                results.push_back(std::move(result));
                hashes.push_back(it->plan_hash);
            }
            print_bench_table(mode == "random" ? "RANDOM" : "SEQUENTIAL",
                              sel_axes, results, cache_name, mode, hashes, output_sync);
            phase_results.by_mode.emplace(mode, std::move(results));
        }
        return true;
    };

    PhaseResults cold_results;
    PhaseResults hot_results;
    if (cache_mode_has_cold(cache_options.mode)) {
        std::string cold_name = cache_options.cold_method == ColdMethod::Scrub
            ? "cold_scrubbed" : "cold_first_touch";
        std::cout << "\nCACHE_PHASE name=" << cold_name << "\n";
        bool scrub_each_case = cache_options.cold_isolation == ColdIsolation::Case;
        if (!scrub_each_case && !run_cold_prepare(cache_options)) return 1;
        if (!run_phase(cold_name, cold_dir, scrub_each_case, cold_results)) return 1;
    }

    if (cache_mode_has_hot(cache_options.mode)) {
        std::cout << "\n[Phase] Warm-up\n";
        warm_up_for_plans(b3d_path, plans, cache_options.warmup_scope,
                          warmup_stride, warmup_memory_mb,
                          batch_read, batch_window_slices,
                          dispatch_strategy);
        std::cout << "CACHE_PHASE name=hot_prefetched\n";
        if (!run_phase("hot_prefetched", hot_dir, false, hot_results)) return 1;
    }

    if (cache_mode_has_cold(cache_options.mode) && cache_mode_has_hot(cache_options.mode)) {
        for (const auto& mode : sel_modes) {
            const auto& cold = cold_results.by_mode.at(mode);
            const auto& hot = hot_results.by_mode.at(mode);
            for (size_t i = 0; i < sel_axes.size(); ++i) {
                std::cout << "BENCHMARK_COMPARE mode=" << mode
                          << " axis=" << sel_axes[i]
                          << " observed_ratio="
                          << (hot[i].total_sec > 0.0 ? cold[i].total_sec / hot[i].total_sec : 0.0)
                          << "\n";
            }
        }
        if (!verify_phase_outputs(cold_dir, hot_dir, ds_name, plans)) return 1;
    }

    auto b3d_sz = fs::file_size(b3d_path);
    std::cout << "\n" << std::string(70, '=') << "\n"
              << "  SUMMARY -- " << ds_name << "\n"
              << std::string(70, '=') << "\n";
    if (fs::exists(raw_path)) {
        auto raw_sz = fs::file_size(raw_path);
        double ratio = static_cast<double>(b3d_sz) / static_cast<double>(raw_sz);
        std::cout << "  Storage:   " << size_str(b3d_sz) << " / " << size_str(raw_sz)
                  << " = " << std::fixed << std::setprecision(3) << ratio << "x\n";
    } else {
        std::cout << "  Storage:   " << size_str(b3d_sz)
                  << " (.b3d only; raw file not present)\n";
    }
    std::cout << "  Outputs:   " << run_dir.string() << "\n"
              << std::string(70, '=') << "\n\n";
    return 0;
}

// ── Main ─────────────────────────────────────────────────────────────

static void print_usage(const char* exe) {
    std::cout << "Usage: " << exe << " [options]\n"
              << "  --datasets NAME [NAME ...]\n"
              << "  --axis x|y|z|all --mode random|sequential|all\n"
              << "  --random-count N --seq-count N --seq-start N --seed N\n"
              << "  --cache-mode cold|hot|both\n"
              << "  --batch-read legacy|fused --batch-window N\n"
              << "  --pipeline off|on (default on) --pipeline-memory MB (default 256) --pipeline-window N (0=auto)\n"
              << "  --read-dispatch round-robin|contiguous\n"
              << "  --cold-method scrub|none --cold-scrub-file FILE\n"
              << "  --cold-scrub-ratio R --cold-scrub-passes N --cold-settle-ms N\n"
              << "  --cold-isolation suite|case --warmup-scope dataset|workload\n"
              << "  --warm-up (compatibility alias for --cache-mode hot)\n"
              << "  --warmup-stride N --warmup-memory MB\n"
              << "  --output-dir DIR --no-output-sync --no-log\n"
              << "  --b3d-file FILE (explicit .b3d path; requires a single dataset)\n"
              << "  --verify --verify-samples N --dim-x N --dim-y N --dim-z N\n";
}

int main(int argc, char* argv[]) {
    std::vector<std::string> datasets;
    uint64_t block_size = 0;   // 0 = auto-detect via storage probe
    int random_count = 100;
    int seq_count = 10;
    int verify_samples = 20000;
    bool verify_enabled = false;   // default: do NOT pollute cache
    bool no_log = false;
    bool warm_up = false;
    bool cache_mode_explicit = false;
    CacheOptions cache_options;
    int warmup_stride_val = 4096;
    int warmup_memory_mb = 0;
    uint32_t seed = 42;
    uint64_t seq_start = 0;
    std::string output_dir;  // empty -> derive (cwd/benchmark_output)
    bool output_sync = true; // default: request persistence
    std::string axis_arg = "all";
    std::string mode_arg = "all";
    std::string batch_read = "fused";
    int batch_window_arg = 4;
    std::string pipeline = "on";
    int pipeline_memory_mb = 256;
    int pipeline_window_arg = 0;
    std::string read_dispatch = "round-robin";
    std::string explicit_b3d_file;
    uint64_t custom_dx = 0, custom_dy = 0, custom_dz = 0;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--datasets" && i + 1 < argc) {
            while (++i < argc && argv[i][0] != '-')
                datasets.push_back(argv[i]);
            i--;
        } else if (arg == "--block-size" && i + 1 < argc) {
            block_size = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--random-count" && i + 1 < argc) {
            random_count = std::atoi(argv[++i]);
        } else if (arg == "--seq-count" && i + 1 < argc) {
            seq_count = std::atoi(argv[++i]);
        } else if (arg == "--seq-start" && i + 1 < argc) {
            seq_start = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--seed" && i + 1 < argc) {
            seed = static_cast<uint32_t>(
                        std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--output-dir" && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (arg == "--verify-samples" && i + 1 < argc) {
            verify_samples = std::atoi(argv[++i]);
        } else if (arg == "--verify") {
            verify_enabled = true;
        } else if (arg == "--skip-verify") {
            verify_enabled = false;  // compat; also the default
        } else if (arg == "--no-output-sync") {
            output_sync = false;
        } else if (arg == "--axis" && i + 1 < argc) {
            axis_arg = argv[++i];
        } else if (arg == "--mode" && i + 1 < argc) {
            mode_arg = argv[++i];
        } else if (arg == "--batch-read" && i + 1 < argc) {
            batch_read = argv[++i];
        } else if (arg == "--batch-window" && i + 1 < argc) {
            batch_window_arg = std::atoi(argv[++i]);
        } else if (arg == "--pipeline" && i + 1 < argc) {
            pipeline = argv[++i];
        } else if (arg == "--pipeline-memory" && i + 1 < argc) {
            pipeline_memory_mb = std::atoi(argv[++i]);
        } else if (arg == "--pipeline-window" && i + 1 < argc) {
            pipeline_window_arg = std::atoi(argv[++i]);
        } else if (arg == "--read-dispatch" && i + 1 < argc) {
            read_dispatch = argv[++i];
        } else if (arg == "--b3d-file" && i + 1 < argc) {
            explicit_b3d_file = argv[++i];
        } else if (arg == "--no-log") {
            no_log = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--warm-up") {
            warm_up = true;
        } else if (arg == "--cache-mode" && i + 1 < argc) {
            cache_mode_explicit = true;
            if (!parse_cache_mode(argv[++i], cache_options.mode)) {
                std::cerr << "[ERROR] invalid --cache-mode: must be cold|hot|both\n";
                return 2;
            }
        } else if (arg == "--cold-method" && i + 1 < argc) {
            if (!parse_cold_method(argv[++i], cache_options.cold_method)) {
                std::cerr << "[ERROR] invalid --cold-method: must be scrub|none\n";
                return 2;
            }
        } else if (arg == "--cold-scrub-file" && i + 1 < argc) {
            cache_options.cold_scrub_file = argv[++i];
        } else if (arg == "--cold-scrub-ratio" && i + 1 < argc) {
            cache_options.cold_scrub_ratio = std::strtod(argv[++i], nullptr);
        } else if (arg == "--cold-scrub-passes" && i + 1 < argc) {
            cache_options.cold_scrub_passes = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--cold-settle-ms" && i + 1 < argc) {
            cache_options.cold_settle_ms = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--cold-isolation" && i + 1 < argc) {
            if (!parse_cold_isolation(argv[++i], cache_options.cold_isolation)) {
                std::cerr << "[ERROR] invalid --cold-isolation: must be suite|case\n";
                return 2;
            }
        } else if (arg == "--warmup-scope" && i + 1 < argc) {
            if (!parse_warmup_scope(argv[++i], cache_options.warmup_scope)) {
                std::cerr << "[ERROR] invalid --warmup-scope: must be dataset|workload\n";
                return 2;
            }
        } else if (arg == "--warmup-stride" && i + 1 < argc) {
            warmup_stride_val = std::atoi(argv[++i]);
        } else if (arg == "--warmup-memory" && i + 1 < argc) {
            warmup_memory_mb = std::atoi(argv[++i]);
        } else if (arg == "--dim-x" && i + 1 < argc) {
            custom_dx = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--dim-y" && i + 1 < argc) {
            custom_dy = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--dim-z" && i + 1 < argc) {
            custom_dz = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::cerr << "[ERROR] unknown or incomplete argument: " << arg << "\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    if (datasets.empty()) datasets = {"test18", "test50"};
    if (!explicit_b3d_file.empty() && datasets.size() != 1) {
        std::cerr << "[ERROR] --b3d-file requires exactly one dataset in --datasets\n";
        return 2;
    }
    if (warm_up) {
        if (cache_mode_explicit && cache_options.mode != CacheMode::Hot) {
            std::cerr << "[ERROR] --warm-up is a compatibility alias for --cache-mode hot and conflicts with cold/both\n";
            return 2;
        }
        cache_options.mode = CacheMode::Hot;
    }

    // ── Strict parameter validation (fail fast, non-zero) ────────────
    // block_size == 0 means auto-detect; resolved per-dataset in run_full_test.
    if (block_size > 256) {
        std::cerr << "[ERROR] --block-size must be 0–256 (0=auto)\n";
        return 2;
    }
    if (random_count <= 0) {
        std::cerr << "[ERROR] --random-count must be > 0 (got "
                  << random_count << ")\n";
        return 2;
    }
    if (seq_count <= 0) {
        std::cerr << "[ERROR] --seq-count must be > 0 (got "
                  << seq_count << ")\n";
        return 2;
    }
    if (warmup_memory_mb < 0) {
        std::cerr << "[ERROR] --warmup-memory must be >= 0 (got "
                  << warmup_memory_mb << ")\n";
        return 2;
    }
    if (cache_mode_has_hot(cache_options.mode) && warmup_stride_val < 1) {
        std::cerr << "[ERROR] --warmup-stride must be >= 1 when warming up (got "
                  << warmup_stride_val << ")\n";
        return 2;
    }
    if (cache_options.cold_scrub_ratio <= 0.0) {
        std::cerr << "[ERROR] --cold-scrub-ratio must be > 0\n";
        return 2;
    }
    if (cache_options.cold_scrub_passes == 0) {
        std::cerr << "[ERROR] --cold-scrub-passes must be > 0\n";
        return 2;
    }
    if (cache_mode_has_cold(cache_options.mode) &&
        cache_options.cold_method == ColdMethod::Scrub &&
        cache_options.cold_scrub_file.empty()) {
        std::cerr << "[ERROR] --cold-scrub-file is required when cold scrub is enabled\n";
        return 2;
    }
    if (batch_read != "legacy" && batch_read != "fused") {
        std::cerr << "[ERROR] invalid --batch-read: must be legacy|fused\n";
        return 2;
    }
    if (batch_window_arg <= 0) {
        std::cerr << "[ERROR] --batch-window must be > 0\n";
        return 2;
    }
    if (pipeline != "off" && pipeline != "on") {
        std::cerr << "[ERROR] invalid --pipeline: must be off|on\n";
        return 2;
    }
    if (pipeline_memory_mb <= 0) {
        std::cerr << "[ERROR] --pipeline-memory must be > 0\n";
        return 2;
    }
    if (pipeline_window_arg < 0) {
        std::cerr << "[ERROR] --pipeline-window must be >= 0 (0=auto)\n";
        return 2;
    }
    if (read_dispatch != "round-robin" && read_dispatch != "contiguous") {
        std::cerr << "[ERROR] invalid --read-dispatch: must be round-robin|contiguous\n";
        return 2;
    }
    ReadDispatchStrategy dispatch_strategy = parse_dispatch_strategy(read_dispatch);

    std::vector<char> sel_axes;
    if (axis_arg == "all")      sel_axes = {'x', 'y', 'z'};
    else if (axis_arg == "x")   sel_axes = {'x'};
    else if (axis_arg == "y")   sel_axes = {'y'};
    else if (axis_arg == "z")   sel_axes = {'z'};
    else {
        std::cerr << "[ERROR] invalid --axis '" << axis_arg
                  << "': must be x|y|z|all\n";
        return 2;
    }

    std::vector<std::string> sel_modes;
    if (mode_arg == "all")           sel_modes = {"random", "sequential"};
    else if (mode_arg == "random")  sel_modes = {"random"};
    else if (mode_arg == "sequential") sel_modes = {"sequential"};
    else {
        std::cerr << "[ERROR] invalid --mode '" << mode_arg
                  << "': must be random|sequential|all\n";
        return 2;
    }

    if (verify_enabled && verify_samples <= 0) {
        std::cerr << "[ERROR] --verify requested but --verify-samples="
                  << verify_samples << " is not positive\n";
        return 2;
    }

    auto cwd     = fs::current_path();
    auto exe_dir = fs::path(argv[0]).parent_path();

    // Resolve file paths
    auto resolve = [&](DatasetInfo& cfg) {
        cfg.raw = resolve_path(cfg.raw, cwd, exe_dir);
        cfg.b3d = resolve_path(cfg.b3d, cwd, exe_dir);
    };
    auto resolve_b3d_arg = [&]() -> std::string {
        fs::path p(explicit_b3d_file);
        if (p.is_absolute()) return p.string();
        return resolve_path(explicit_b3d_file, cwd, exe_dir);
    };

    // ── Unique run id (ms timestamp + pid) and unique shared run dir ──
    // Bump suffix until BOTH the output dir and (if logging) the log file
    // path do not already exist; never reuse/truncate a prior run's files.
    std::string base_id = make_run_id();
    fs::path out_root = output_dir.empty()
        ? (cwd / "benchmark_output")
        : fs::path(output_dir);
    auto log_candidate = [&](const std::string& id) -> fs::path {
        std::ostringstream oss;
        oss << "logs/block3d_";
        for (size_t i = 0; i < datasets.size(); i++) {
            if (i > 0) oss << "_";
            oss << datasets[i];
        }
        oss << "_" << id << ".log";
        return fs::path(oss.str());
    };
    std::string run_id = base_id;
    int suffix = 2;
    for (;;) {
        fs::path cand_dir = out_root / run_id;
        bool exists_dir = fs::exists(cand_dir);
        bool exists_log = (!no_log) && fs::exists(log_candidate(run_id));
        if (!exists_dir && !exists_log) break;
        run_id = base_id + "_" + std::to_string(suffix++);
        if (suffix > 9999) { // sanity guard
            std::cerr << "[ERROR] could not allocate a unique run id\n";
            return 1;
        }
    }
    g_run_id = run_id;
    fs::path run_dir = out_root / run_id;

    // Logging
    if (!no_log) setup_logging(datasets);

    // Run-wide notices
    std::cout << "CACHE_CONFIG mode=" << to_string(cache_options.mode)
              << " cold_method=" << to_string(cache_options.cold_method)
              << " cold_isolation=" << to_string(cache_options.cold_isolation)
              << " warmup_scope=" << to_string(cache_options.warmup_scope)
              << " timing_scope=read_write_total\n";
    std::cout << "BATCH_READ mode=" << batch_read
              << " window_slices=" << batch_window_arg << "\n";
    PipelineConfig pipeline_config;
    pipeline_config.enabled = pipeline == "on";
    pipeline_config.memory_mb = static_cast<uint64_t>(pipeline_memory_mb);
    pipeline_config.window_slices = static_cast<size_t>(pipeline_window_arg);

    std::cout << "PIPELINE mode=" << pipeline
              << " buffer_mb=" << (pipeline_config.enabled ? pipeline_config.memory_mb : 0)
              << " window_slices=" << (pipeline_config.enabled ? pipeline_config.window_slices : 0)
              << " queue_slices=auto writer_threads=" << (pipeline_config.enabled ? 1 : 0)
              << "\n";
    std::cout << "READ_DISPATCH strategy=" << normalize_dispatch(read_dispatch) << "\n";
    std::cout << "[OUTPUT-SYNC] output_sync="
              << (output_sync ? "requested" : "disabled") << "\n";
    std::cout << "[CASE-PLAN] axis=" << axis_arg << " mode=" << mode_arg
              << " cold_isolation=" << to_string(cache_options.cold_isolation) << "\n";
    std::cout << "[VERIFY-MODE] verify="
              << (verify_enabled ? "enabled" : "skipped") << "\n\n";

    // Create the shared run directory now (unique id guarantees no reuse).
    {
        std::error_code ec;
        fs::create_directories(run_dir, ec);
        if (ec) {
            std::cerr << "[ERROR] cannot create run directory '" << run_dir
                      << "': " << ec.message() << "\n";
            teardown_logging();
            return 1;
        }
    }

    auto t_start = std::chrono::high_resolution_clock::now();

    int ret = 0;
    for (const auto& ds_name : datasets) {
        DatasetInfo cfg;
        auto it = DATASETS.find(ds_name);
        if (it != DATASETS.end()) {
            cfg = it->second;
        } else {
            // Custom dataset: requires --dim-x/--dim-y/--dim-z.
            if (custom_dx == 0 || custom_dy == 0 || custom_dz == 0) {
                std::cout << "[ERROR] Unknown dataset: " << ds_name
                          << ". Available:";
                for (const auto& [k, v] : DATASETS) std::cout << " " << k;
                std::cout << "\n"
                          << "        For custom datasets, specify"
                          << " --dim-x N --dim-y N --dim-z N\n";
                ret = 1;
                break;
            }
            cfg.raw   = ds_name + ".dat";
            cfg.b3d   = ds_name + ".b3d";
            cfg.dim_x = custom_dx;
            cfg.dim_y = custom_dy;
            cfg.dim_z = custom_dz;
        }
        resolve(cfg);
        if (!explicit_b3d_file.empty()) {
            cfg.b3d = resolve_b3d_arg();
            if (!fs::exists(cfg.b3d)) {
                std::cout << "[ERROR] explicit .b3d file not found: " << cfg.b3d << "\n";
                ret = 1;
                break;
            }
            try {
                BlockedFileReader reader(cfg.b3d);
                cfg.dim_x = reader.dim_x();
                cfg.dim_y = reader.dim_y();
                cfg.dim_z = reader.dim_z();
            } catch (const std::exception& e) {
                std::cout << "[ERROR] cannot read explicit .b3d file '" << cfg.b3d
                          << "': " << e.what() << "\n";
                ret = 1;
                break;
            }
        } else if (!fs::exists(cfg.raw)) {
            std::cout << "[ERROR] raw file not found for dataset '" << ds_name
                      << "': " << cfg.raw << "\n";
            ret = 1;
            break;
        }

        ret = run_full_test(ds_name, cfg, block_size,
                            random_count, seq_count,
                            verify_enabled, verify_samples,
                            cache_options,
                            static_cast<size_t>(warmup_stride_val),
                            static_cast<uint64_t>(warmup_memory_mb),
                            seed,
                            seq_start,
                            run_dir,
                            sel_axes,
                            sel_modes,
                            output_sync,
                            batch_read,
                            static_cast<size_t>(batch_window_arg),
                            pipeline_config,
                            dispatch_strategy,
                            !explicit_b3d_file.empty());
        if (ret != 0) break;
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    double total_dt = std::chrono::duration<double>(t_end - t_start).count();
    std::cout << "Total elapsed: " << std::fixed
              << std::setprecision(1) << total_dt << "s\n";

    teardown_logging();
    return ret;
}
