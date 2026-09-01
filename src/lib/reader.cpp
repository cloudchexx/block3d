#include "block3d/reader.hpp"
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <exception>
#include <type_traits>
#include <utility>

#ifdef OMP_ENABLED
#include <omp.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace block3d {

// ── MappedFile ────────────────────────────────────────────────────────

MappedFile::MappedFile(const std::string& path) { do_mmap(path); }
MappedFile::~MappedFile() { close(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : mapped_data_(other.mapped_data_), size_(other.size_)
#ifdef _WIN32
      , file_handle_(other.file_handle_), map_handle_(other.map_handle_)
#else
      , fd_(other.fd_)
#endif
{
    other.mapped_data_ = nullptr;
#ifdef _WIN32
    other.file_handle_ = nullptr;
    other.map_handle_  = nullptr;
#else
    other.fd_ = -1;
#endif
    other.size_ = 0;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close();
        mapped_data_ = other.mapped_data_;
        size_        = other.size_;
#ifdef _WIN32
        file_handle_ = other.file_handle_;
        map_handle_  = other.map_handle_;
        other.file_handle_ = nullptr;
        other.map_handle_  = nullptr;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
        other.mapped_data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

void MappedFile::close() {
#ifdef _WIN32
    if (mapped_data_) { UnmapViewOfFile(mapped_data_); mapped_data_ = nullptr; }
    if (map_handle_)  { CloseHandle(map_handle_);       map_handle_  = nullptr; }
    if (file_handle_ && file_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_handle_); file_handle_ = nullptr;
    }
#else
    if (mapped_data_ && mapped_data_ != MAP_FAILED) {
        munmap(mapped_data_, size_);
        mapped_data_ = nullptr;
    }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
    size_ = 0;
}

void MappedFile::do_mmap(const std::string& path) {
#ifdef _WIN32
    HANDLE hFile = CreateFileA(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Failed to open file: " + path);
    file_handle_ = hFile;

    LARGE_INTEGER li;
    GetFileSizeEx(hFile, &li);
    size_ = static_cast<size_t>(li.QuadPart);

    HANDLE hMap = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) {
        CloseHandle(hFile); file_handle_ = nullptr;
        throw std::runtime_error("Failed to create file mapping: " + path);
    }
    map_handle_ = hMap;

    void* ptr = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!ptr) {
        CloseHandle(hMap);   map_handle_ = nullptr;
        CloseHandle(hFile);  file_handle_ = nullptr;
        throw std::runtime_error("Failed to map view: " + path);
    }
    mapped_data_ = ptr;
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        throw std::runtime_error("Failed to open file: " + path);
    fd_ = fd;

    struct stat st;
    fstat(fd, &st);
    size_ = static_cast<size_t>(st.st_size);

    void* ptr = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    if (ptr == MAP_FAILED) {
        ::close(fd); fd_ = -1;
        throw std::runtime_error("Failed to mmap file: " + path);
    }
    mapped_data_ = ptr;
#endif
}

void MappedFile::prefault(size_t offset, size_t length, size_t stride) {
    if (!mapped_data_ || offset >= size_) return;
    if (offset + length > size_) length = size_ - offset;
    if (length == 0 || stride == 0) return;

    const uint8_t* base = reinterpret_cast<const uint8_t*>(mapped_data_) + offset;

#ifdef _WIN32
    WIN32_MEMORY_RANGE_ENTRY entry;
    entry.VirtualAddress = const_cast<void*>(static_cast<const void*>(base));
    entry.NumberOfBytes  = length;
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &entry, 0);
#else
    ::madvise(static_cast<char*>(mapped_data_) + offset,
              length, MADV_WILLNEED);
#endif

    // PrefetchVirtualMemory/madvise are hints only. Complete the warm-up
    // contract by synchronously touching every requested stride and the final
    // byte before returning.
    volatile uint8_t sink = 0;
    for (size_t pos = 0; pos < length; pos += stride) {
        sink ^= base[pos];
    }
    sink ^= base[length - 1];
    (void)sink;
}

// ── Intra-slice block-processing helper ───────────────────────────────
// Blocks within a single slice write to non-overlapping regions of the
// output buffer, so they can safely run in parallel without locks.
// Falls back to serial when block_count ≤ num_threads × 4.

class BlockedFileReader::ThreadPool {
public:
    explicit ThreadPool(int num_threads) {
        size_t nw = num_threads > 1 ? static_cast<size_t>(num_threads) : 0;
        workers_.reserve(nw);
        for (size_t id = 0; id < nw; id++) {
            workers_.emplace_back([this, id] { worker_loop(id); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            generation_++;
        }
        start_cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

    size_t worker_count() const { return workers_.size(); }
    uint64_t jobs() const { return jobs_.load(std::memory_order_relaxed); }
    uint64_t serial_fallbacks() const {
        return serial_fallbacks_.load(std::memory_order_relaxed);
    }

    void record_serial_fallback() {
        serial_fallbacks_.fetch_add(1, std::memory_order_relaxed);
    }

    template<typename F>
    void parallel_for(size_t max_workers,
                      ReadDispatchStrategy strategy,
                      const std::vector<BlockCoord>& blocks,
                      F&& process) {
        if (workers_.empty() || max_workers <= 1 || in_worker_thread_) {
            serial_fallbacks_.fetch_add(1, std::memory_order_relaxed);
            for (const auto& block : blocks) process(block);
            return;
        }

        using Fn = typename std::remove_reference<F>::type;
        Fn* fn = &process;
        auto invoke = [](void* ctx, const BlockCoord& block) {
            (*static_cast<Fn*>(ctx))(block);
        };

        std::unique_lock<std::mutex> lock(mutex_);
        idle_cv_.wait(lock, [&] { return !active_; });

        active_ = true;
        blocks_ = &blocks;
        context_ = fn;
        invoke_ = invoke;
        strategy_ = strategy;
        active_worker_count_ = std::min({workers_.size(), blocks.size(), max_workers});
        completed_workers_ = 0;
        exception_ = nullptr;
        cancelled_.store(false, std::memory_order_release);
        jobs_.fetch_add(1, std::memory_order_relaxed);
        generation_++;

        start_cv_.notify_all();
        done_cv_.wait(lock, [&] { return completed_workers_ == active_worker_count_; });

        std::exception_ptr exception = exception_;
        active_ = false;
        blocks_ = nullptr;
        context_ = nullptr;
        invoke_ = nullptr;
        strategy_ = ReadDispatchStrategy::RoundRobin;
        active_worker_count_ = 0;
        idle_cv_.notify_one();

        if (exception) std::rethrow_exception(exception);
    }

private:
    using InvokeFn = void (*)(void*, const BlockCoord&);

    static thread_local bool in_worker_thread_;

    void worker_loop(size_t worker_id) {
        in_worker_thread_ = true;
        uint64_t seen_generation = 0;
        for (;;) {
            const std::vector<BlockCoord>* blocks = nullptr;
            void* context = nullptr;
            InvokeFn invoke = nullptr;
            size_t worker_count = 0;
            ReadDispatchStrategy strategy = ReadDispatchStrategy::RoundRobin;
            uint64_t job_generation = 0;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                start_cv_.wait(lock, [&] {
                    return stopping_ || generation_ != seen_generation;
                });
                if (stopping_) break;
                seen_generation = generation_;
                if (!active_ || worker_id >= active_worker_count_) continue;

                blocks = blocks_;
                context = context_;
                invoke = invoke_;
                worker_count = active_worker_count_;
                strategy = strategy_;
                job_generation = generation_;
            }

            try {
                if (strategy == ReadDispatchStrategy::Contiguous) {
                    size_t chunk = (blocks->size() + worker_count - 1) / worker_count;
                    size_t begin = worker_id * chunk;
                    size_t end = std::min(blocks->size(), begin + chunk);
                    for (size_t i = begin; i < end; i++) {
                        if (cancelled_.load(std::memory_order_acquire)) break;
                        invoke(context, (*blocks)[i]);
                    }
                } else {
                    for (size_t i = worker_id; i < blocks->size(); i += worker_count) {
                        if (cancelled_.load(std::memory_order_acquire)) break;
                        invoke(context, (*blocks)[i]);
                    }
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (job_generation == generation_ && !exception_) {
                    exception_ = std::current_exception();
                    cancelled_.store(true, std::memory_order_release);
                }
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (job_generation == generation_) {
                    completed_workers_++;
                    if (completed_workers_ == active_worker_count_) done_cv_.notify_one();
                }
            }
        }
        in_worker_thread_ = false;
    }

    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    std::condition_variable start_cv_;
    std::condition_variable done_cv_;
    std::condition_variable idle_cv_;

    bool stopping_ = false;
    bool active_ = false;
    uint64_t generation_ = 0;
    size_t active_worker_count_ = 0;
    size_t completed_workers_ = 0;
    const std::vector<BlockCoord>* blocks_ = nullptr;
    void* context_ = nullptr;
    InvokeFn invoke_ = nullptr;
    ReadDispatchStrategy strategy_ = ReadDispatchStrategy::RoundRobin;
    std::exception_ptr exception_;
    std::atomic<bool> cancelled_{false};
    std::atomic<uint64_t> jobs_{0};
    std::atomic<uint64_t> serial_fallbacks_{0};
};

thread_local bool BlockedFileReader::ThreadPool::in_worker_thread_ = false;

template<typename F>
void BlockedFileReader::for_each_block_parallel(
        int requested_threads,
        const std::vector<BlockCoord>& blocks,
        F&& process) {
    size_t nb = blocks.size();
    int nt = requested_threads > 0 ? requested_threads : num_threads_;
    if (nt <= 1 || nb <= static_cast<size_t>(nt) * 4) {
        if (thread_pool_) thread_pool_->record_serial_fallback();
        for (size_t i = 0; i < nb; i++) process(blocks[i]);
        return;
    }
    if (!thread_pool_) {
        for (size_t i = 0; i < nb; i++) process(blocks[i]);
        return;
    }
    size_t max_workers = static_cast<size_t>(nt);
    thread_pool_->parallel_for(max_workers, dispatch_strategy_, blocks, std::forward<F>(process));
}

template<typename F>
void BlockedFileReader::for_each_block_parallel(
        const std::vector<BlockCoord>& blocks,
        F&& process) {
    for_each_block_parallel(num_threads_, blocks, std::forward<F>(process));
}

// ── BlockedFileReader ─────────────────────────────────────────────────

BlockedFileReader::BlockedFileReader(const std::string& file_path,
                                     int num_threads,
                                     uint64_t max_memory_mb,
                                     ReadDispatchStrategy dispatch_strategy)
    : num_threads_(num_threads > 0 ? num_threads
                                   : static_cast<int>(
                                         std::thread::hardware_concurrency())),
      max_memory_mb_(max_memory_mb),
      dispatch_strategy_(dispatch_strategy)
{
    if (num_threads_ < 1) num_threads_ = 1;

    std::ifstream f(file_path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open file: " + file_path);

    FileHeader hdr;
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (f.gcount() != sizeof(hdr))
        throw std::runtime_error("Failed to read header");

    if (std::memcmp(hdr.magic, MAGIC, 4) != 0)
        throw std::runtime_error("Invalid file magic");

    if (hdr.version == VERSION_LEGACY) {
        if (hdr.reserved != 0) {
            throw std::runtime_error("Unsupported v1 layout metadata");
        }
        inner_layout_ = BlockInnerLayout::LegacyXYZ;
        micro_size_ = 0;
    } else if (hdr.version == VERSION_MICROTILE) {
        uint8_t layout_kind = header_layout_kind(hdr.reserved);
        uint8_t micro_size = header_micro_size(hdr.reserved);
        uint64_t reserved_high = hdr.reserved & ~0xffffULL;
        if (layout_kind != LAYOUT_MICRO_TILED_XYZ ||
            micro_size != DEFAULT_MICRO_SIZE ||
            reserved_high != 0) {
            throw std::runtime_error("Unsupported v2 layout metadata");
        }
        if (hdr.block_size % micro_size != 0) {
            throw std::runtime_error("Invalid micro-tiled block_size/micro_size");
        }
        inner_layout_ = BlockInnerLayout::MicroTiledXYZ;
        micro_size_ = micro_size;
    } else {
        throw std::runtime_error("Unsupported file version");
    }

    version_ = hdr.version;
    dim_x_ = hdr.dim_x;
    dim_y_ = hdr.dim_y;
    dim_z_ = hdr.dim_z;
    block_size_ = hdr.block_size;
    total_blocks_ = hdr.total_blocks;
    data_offset_ = hdr.data_offset;
    layout_ = BlockLayout3D(dim_x_, dim_y_, dim_z_, block_size_);

    block_offsets_.resize(total_blocks_);
    f.seekg(HEADER_SIZE);
    f.read(reinterpret_cast<char*>(block_offsets_.data()),
           total_blocks_ * sizeof(uint64_t));

    mapped_file_ = MappedFile(file_path);
    if (num_threads_ > 1) {
        thread_pool_ = std::make_unique<ThreadPool>(num_threads_);
    }
}

BlockedFileReader::~BlockedFileReader() = default;

size_t BlockedFileReader::thread_pool_workers() const {
    return thread_pool_ ? thread_pool_->worker_count() : 0;
}

uint64_t BlockedFileReader::thread_pool_jobs() const {
    return thread_pool_ ? thread_pool_->jobs() : 0;
}

uint64_t BlockedFileReader::thread_pool_serial_fallbacks() const {
    return thread_pool_ ? thread_pool_->serial_fallbacks() : 0;
}

void BlockedFileReader::warm_up(bool async, size_t stride,
                                 uint64_t max_memory_mb) {
    if (warm_up_future_.valid() &&
        !warm_up_done_.load(std::memory_order_acquire)) return;

    uint64_t bs = block_size_;
    uint64_t data_size = total_blocks_ * bs * bs * bs * sizeof(float);
    uint64_t mapped_data_size = mapped_file_.size() > data_offset_
        ? static_cast<uint64_t>(mapped_file_.size() - data_offset_)
        : 0;
    uint64_t prefault_size = std::min(data_size, mapped_data_size);
    if (max_memory_mb > 0) {
        uint64_t max_bytes = max_memory_mb * 1024ULL * 1024ULL;
        if (prefault_size > max_bytes) prefault_size = max_bytes;
    }

    auto prefault_fn = [this, stride, prefault_size]() {
        mapped_file_.prefault(data_offset_, prefault_size, stride);
        warm_up_done_.store(true, std::memory_order_release);
    };

    if (async) {
        warm_up_done_.store(false, std::memory_order_release);
        warm_up_future_ = std::async(std::launch::async, prefault_fn);
    } else {
        warm_up_done_.store(false, std::memory_order_release);
        prefault_fn();
    }
}

bool BlockedFileReader::is_warm_up_done() const {
    return warm_up_done_.load(std::memory_order_acquire);
}

void BlockedFileReader::wait_warm_up() {
    if (warm_up_future_.valid()) warm_up_future_.wait();
}

uint64_t
BlockedFileReader::block_offset_float(uint32_t bx, uint32_t by_,
                                       uint32_t bz) const {
    uint64_t lin = layout_.linear_index(bx, by_, bz);
    if (lin >= block_offsets_.size()) {
        throw std::out_of_range("block_offset_float: linear index out of range");
    }
    return block_offsets_[lin] / 4;
}

uint64_t BlockedFileReader::local_offset(uint32_t lx, uint32_t ly, uint32_t lz) const {
    return local_offset_for_layout(inner_layout_, block_size_, micro_size_, lx, ly, lz);
}

void BlockedFileReader::copy_z_run(const float* data,
                                   uint64_t block_float_offset,
                                   uint32_t lx, uint32_t ly,
                                   uint32_t lz_start, uint32_t lz_end,
                                   float* dst) const {
    if (lz_start >= lz_end) return;
    if (inner_layout_ == BlockInnerLayout::LegacyXYZ) {
        const float* src = data + block_float_offset
            + legacy_local_offset(block_size_, lx, ly, lz_start);
        std::memcpy(dst, src, static_cast<size_t>(lz_end - lz_start) * sizeof(float));
        return;
    }

    const uint32_t m = micro_size_;
    uint32_t lz = lz_start;
    while (lz < lz_end) {
        uint32_t next = std::min<uint32_t>(lz_end, ((lz / m) + 1) * m);
        const float* src = data + block_float_offset + local_offset(lx, ly, lz);
        std::memcpy(dst + (lz - lz_start), src, static_cast<size_t>(next - lz) * sizeof(float));
        lz = next;
    }
}

void BlockedFileReader::cache_prune() {
    while (cache_.size() >= CACHE_MAX_ENTRIES && !cache_order_.empty()) {
        CacheKey oldest = cache_order_.front();
        cache_order_.pop_front();
        cache_.erase(oldest);
    }
}

std::vector<BlockCoord>
BlockedFileReader::sorted_block_list(char axis, uint64_t index) {
    uint64_t bs = block_size_;
    uint32_t block_key = static_cast<uint32_t>(index / bs);

    CacheKey key{axis, block_key};

    // Fast path: cache hit. Copy the cached vector *out while holding the
    // lock*, then return that independent copy. The caller may iterate it
    // after the lock is released, so we must never hand back a reference into
    // cache_ (a rehash, prune, or concurrent overwrite of the same key would
    // dangle it).
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            cache_order_.remove(key);
            cache_order_.push_back(key);
            return it->second;  // copy under lock
        }
    }

    // Slow path: build the sorted block list as pure local work (no shared
    // state touched). The check-and-insert is intentionally NOT atomic across
    // the build, so multiple threads may build the same key concurrently;
    // that is harmless because each returns its own local copy and the
    // insert path below is guarded and idempotent.
    std::vector<BlockCoord> coords;

    if (axis == 'x') {
        for (uint32_t by_ = 0; by_ < layout_.blocks_y; by_++)
            for (uint32_t bz = 0; bz < layout_.blocks_z; bz++)
                coords.push_back({block_key, by_, bz});
    } else if (axis == 'y') {
        for (uint32_t bx = 0; bx < layout_.blocks_x; bx++)
            for (uint32_t bz = 0; bz < layout_.blocks_z; bz++)
                coords.push_back({bx, block_key, bz});
    } else {
        for (uint32_t bx = 0; bx < layout_.blocks_x; bx++)
            for (uint32_t by_ = 0; by_ < layout_.blocks_y; by_++)
                coords.push_back({bx, by_, block_key});
    }

    std::sort(coords.begin(), coords.end(),
              [this](const BlockCoord& a, const BlockCoord& b) {
                  return block_offset_float(a.bx, a.by_, a.bz) <
                         block_offset_float(b.bx, b.by_, b.bz);
              });

    // Publish under the lock. Re-check first: another thread may have
    // inserted the same key while we were building. If so, refresh its LRU
    // position and return the existing (deterministic) content instead of
    // clobbering it -- this avoids a redundant overwrite of a vector that a
    // third thread may currently be copying out.
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            cache_order_.remove(key);
            cache_order_.push_back(key);
            return it->second;  // copy under lock
        }
        cache_prune();
        // Store a copy in the cache, keep our local for the return value so
        // we don't need a second copy out of the map.
        cache_[key] = coords;
        cache_order_.push_back(key);
        return coords;  // move/copy of our local; fully independent of cache_
    }
}

// ── X-Slice ──────────────────────────────────────────────────────────

std::vector<float> BlockedFileReader::read_x_slice(uint64_t x) {
    if (x >= dim_x_)
        throw std::out_of_range("X out of range");

    uint64_t ny = dim_y_, nz = dim_z_;
    uint64_t bs = block_size_;
    const float* dv = mapped_file_.data();

    std::vector<float> result(ny * nz);
    uint32_t bx = static_cast<uint32_t>(x / bs);
    uint32_t lx = static_cast<uint32_t>(x % bs);

    auto blocks = sorted_block_list('x', x);

    for_each_block_parallel(blocks,
        [&](const BlockCoord& c) {
            if (c.bx != bx) return;

            uint64_t y0 = static_cast<uint64_t>(c.by_) * bs;
            uint64_t z0 = static_cast<uint64_t>(c.bz) * bs;
            uint64_t ny_part = std::min(bs, dim_y_ - y0);
            uint64_t nz_part = std::min(bs, dim_z_ - z0);

            uint64_t boff = block_offset_float(c.bx, c.by_, c.bz);

            for (uint32_t ly = 0; ly < ny_part; ly++) {
                float* dst = &result[(y0 + ly) * nz + z0];
                copy_z_run(dv, boff, lx, ly, 0, static_cast<uint32_t>(nz_part), dst);
            }
        });

    return result;
}

// ── Y-Slice ──────────────────────────────────────────────────────────

std::vector<float> BlockedFileReader::read_y_slice(uint64_t y) {
    if (y >= dim_y_)
        throw std::out_of_range("Y out of range");

    uint64_t nx = dim_x_, nz = dim_z_;
    uint64_t bs = block_size_;
    const float* dv = mapped_file_.data();

    std::vector<float> result(nx * nz);
    uint32_t by_ = static_cast<uint32_t>(y / bs);
    uint32_t ly  = static_cast<uint32_t>(y % bs);

    auto blocks = sorted_block_list('y', y);

    for_each_block_parallel(blocks,
        [&](const BlockCoord& c) {
            if (c.by_ != by_) return;

            uint64_t x0 = static_cast<uint64_t>(c.bx) * bs;
            uint64_t z0 = static_cast<uint64_t>(c.bz) * bs;
            uint64_t nx_part = std::min(bs, dim_x_ - x0);
            uint64_t nz_part = std::min(bs, dim_z_ - z0);

            uint64_t boff = block_offset_float(c.bx, c.by_, c.bz);

            for (uint32_t lx = 0; lx < nx_part; lx++) {
                float* dst = &result[(x0 + lx) * nz + z0];
                copy_z_run(dv, boff, lx, ly, 0, static_cast<uint32_t>(nz_part), dst);
            }
        });

    return result;
}

// ── Z-Slice ──────────────────────────────────────────────────────────

std::vector<float> BlockedFileReader::read_z_slice(uint64_t z) {
    if (z >= dim_z_)
        throw std::out_of_range("Z out of range");

    uint64_t nx = dim_x_, ny = dim_y_;
    uint64_t bs = block_size_;
    const float* dv = mapped_file_.data();

    std::vector<float> result(nx * ny);
    uint32_t bz = static_cast<uint32_t>(z / bs);
    uint32_t lz = static_cast<uint32_t>(z % bs);

    auto blocks = sorted_block_list('z', z);

    for_each_block_parallel(blocks,
        [&](const BlockCoord& c) {
            if (c.bz != bz) return;

            uint64_t x0 = static_cast<uint64_t>(c.bx) * bs;
            uint64_t y0 = static_cast<uint64_t>(c.by_) * bs;
            uint64_t nx_part = std::min(bs, dim_x_ - x0);
            uint64_t ny_part = std::min(bs, dim_y_ - y0);

            uint64_t boff = block_offset_float(c.bx, c.by_, c.bz);

            for (uint32_t lx = 0; lx < nx_part; lx++) {
                uint64_t x_row = (x0 + lx) * ny;
                for (uint32_t ly = 0; ly < ny_part; ly++) {
                    result[x_row + (y0 + ly)] =
                        dv[boff + local_offset(lx, ly, lz)];
                }
            }
        });

    return result;
}

// ── Generic slice ────────────────────────────────────────────────────

std::vector<float>
BlockedFileReader::read_slice(char axis, uint64_t index) {
    switch (axis) {
    case 'x': return read_x_slice(index);
    case 'y': return read_y_slice(index);
    case 'z': return read_z_slice(index);
    default:
        throw std::invalid_argument("Unknown axis: " + str_axis(axis));
    }
}

namespace {

struct SliceRequest {
    size_t request_pos = 0;
    uint64_t index = 0;
    uint32_t layer = 0;
    uint32_t local = 0;
};

} // anonymous namespace

void BlockedFileReader::read_slices_batch_stream(
        char axis,
        const std::vector<uint64_t>& indices,
        const SliceBatchOptions& options,
        const SliceConsumer& consumer) {
    if (!consumer) {
        throw std::invalid_argument("Slice consumer is empty");
    }

    uint64_t dim = 0;
    uint64_t slice_elems = 0;
    switch (axis) {
    case 'x':
        dim = dim_x_;
        slice_elems = dim_y_ * dim_z_;
        break;
    case 'y':
        dim = dim_y_;
        slice_elems = dim_x_ * dim_z_;
        break;
    case 'z':
        dim = dim_z_;
        slice_elems = dim_x_ * dim_y_;
        break;
    default:
        throw std::invalid_argument("Unknown axis: " + str_axis(axis));
    }
    for (uint64_t index : indices) {
        if (index >= dim) {
            throw std::out_of_range("Batch slice index out of range");
        }
    }

    if (indices.empty()) return;

    uint64_t bs = block_size_;
    std::vector<std::vector<SliceRequest>> groups;
    std::unordered_map<uint32_t, size_t> group_by_layer;
    groups.reserve(indices.size());
    for (size_t i = 0; i < indices.size(); i++) {
        SliceRequest req;
        req.request_pos = i;
        req.index = indices[i];
        req.layer = static_cast<uint32_t>(indices[i] / bs);
        req.local = static_cast<uint32_t>(indices[i] % bs);

        auto [it, inserted] = group_by_layer.emplace(req.layer, groups.size());
        if (inserted) groups.emplace_back();
        groups[it->second].push_back(req);
    }

    size_t window_slices = options.window_slices == 0 ? 1 : options.window_slices;
    int nt = options.num_threads > 0 ? options.num_threads : num_threads_;
    if (nt < 1) nt = 1;
    const float* dv = mapped_file_.data();

    for (const auto& group : groups) {
        if (group.empty()) continue;
        auto blocks = sorted_block_list(axis,
                                        static_cast<uint64_t>(group.front().layer) * bs);
        for (size_t start = 0; start < group.size(); start += window_slices) {
            size_t end = std::min(group.size(), start + window_slices);
            std::vector<std::vector<float>> window(end - start,
                                                   std::vector<float>(slice_elems));

            for_each_block_parallel(nt, blocks,
                [&](const BlockCoord& c) {
                    if (axis == 'x') {
                        uint64_t y0 = static_cast<uint64_t>(c.by_) * bs;
                        uint64_t z0 = static_cast<uint64_t>(c.bz) * bs;
                        uint64_t ny_part = std::min(bs, dim_y_ - y0);
                        uint64_t nz_part = std::min(bs, dim_z_ - z0);
                        uint64_t boff = block_offset_float(c.bx, c.by_, c.bz);
                        for (size_t wi = 0; wi < window.size(); wi++) {
                            const SliceRequest& req = group[start + wi];
                            auto& result = window[wi];
                            for (uint32_t ly = 0; ly < ny_part; ly++) {
                                float* dst = &result[(y0 + ly) * dim_z_ + z0];
                                copy_z_run(dv, boff, req.local, ly, 0,
                                           static_cast<uint32_t>(nz_part), dst);
                            }
                        }
                    } else if (axis == 'y') {
                        uint64_t x0 = static_cast<uint64_t>(c.bx) * bs;
                        uint64_t z0 = static_cast<uint64_t>(c.bz) * bs;
                        uint64_t nx_part = std::min(bs, dim_x_ - x0);
                        uint64_t nz_part = std::min(bs, dim_z_ - z0);
                        uint64_t boff = block_offset_float(c.bx, c.by_, c.bz);
                        for (size_t wi = 0; wi < window.size(); wi++) {
                            const SliceRequest& req = group[start + wi];
                            auto& result = window[wi];
                            for (uint32_t lx = 0; lx < nx_part; lx++) {
                                float* dst = &result[(x0 + lx) * dim_z_ + z0];
                                copy_z_run(dv, boff, lx, req.local, 0,
                                           static_cast<uint32_t>(nz_part), dst);
                            }
                        }
                    } else {
                        uint64_t x0 = static_cast<uint64_t>(c.bx) * bs;
                        uint64_t y0 = static_cast<uint64_t>(c.by_) * bs;
                        uint64_t nx_part = std::min(bs, dim_x_ - x0);
                        uint64_t ny_part = std::min(bs, dim_y_ - y0);
                        uint64_t boff = block_offset_float(c.bx, c.by_, c.bz);
                        for (uint32_t lx = 0; lx < nx_part; lx++) {
                            uint64_t x_row = (x0 + lx) * dim_y_;
                            for (uint32_t ly = 0; ly < ny_part; ly++) {
                                uint64_t dst_idx = x_row + (y0 + ly);
                                for (size_t wi = 0; wi < window.size(); wi++) {
                                    const SliceRequest& req = group[start + wi];
                                    window[wi][dst_idx] =
                                        dv[boff + local_offset(lx, ly, req.local)];
                                }
                            }
                        }
                    }
                });

            for (size_t wi = 0; wi < window.size(); wi++) {
                const SliceRequest& req = group[start + wi];
                consumer(req.request_pos, req.index, std::move(window[wi]));
            }
        }
    }
}

std::vector<std::vector<float>>
BlockedFileReader::read_slices_batch(char axis,
                                     const std::vector<uint64_t>& indices,
                                     int num_threads) {
    size_t n = indices.size();
    std::vector<std::vector<float>> results(n);
    SliceBatchOptions options;
    options.num_threads = num_threads;
    options.window_slices = 1;
    read_slices_batch_stream(axis, indices, options,
        [&](size_t request_pos, uint64_t, std::vector<float>&& slice) {
            results[request_pos] = std::move(slice);
        });
    return results;
}

// ── Column reads ─────────────────────────────────────────────────────

std::vector<float> BlockedFileReader::read_x_column(uint64_t y, uint64_t z) {
    if (y >= dim_y_ || z >= dim_z_)
        throw std::out_of_range("Y or Z out of range");

    uint64_t bs = block_size_;
    const float* dv = mapped_file_.data();
    std::vector<float> result(dim_x_);

    uint32_t by_ = static_cast<uint32_t>(y / bs);
    uint32_t bz  = static_cast<uint32_t>(z / bs);
    uint32_t ly  = static_cast<uint32_t>(y % bs);
    uint32_t lz  = static_cast<uint32_t>(z % bs);

    for (uint32_t bx = 0; bx < layout_.blocks_x; bx++) {
        uint64_t x0 = static_cast<uint64_t>(bx) * bs;
        uint64_t nx_part = std::min(bs, dim_x_ - x0);
        uint64_t boff = block_offset_float(bx, by_, bz);

        for (uint32_t lx = 0; lx < nx_part; lx++) {
            result[x0 + lx] = dv[boff + local_offset(lx, ly, lz)];
        }
    }
    return result;
}

std::vector<float> BlockedFileReader::read_y_column(uint64_t x, uint64_t z) {
    if (x >= dim_x_ || z >= dim_z_)
        throw std::out_of_range("X or Z out of range");

    uint64_t bs = block_size_;
    const float* dv = mapped_file_.data();
    std::vector<float> result(dim_y_);

    uint32_t bx = static_cast<uint32_t>(x / bs);
    uint32_t bz = static_cast<uint32_t>(z / bs);
    uint32_t lx = static_cast<uint32_t>(x % bs);
    uint32_t lz = static_cast<uint32_t>(z % bs);

    for (uint32_t by_ = 0; by_ < layout_.blocks_y; by_++) {
        uint64_t y0 = static_cast<uint64_t>(by_) * bs;
        uint64_t ny_part = std::min(bs, dim_y_ - y0);
        uint64_t boff = block_offset_float(bx, by_, bz);

        for (uint32_t ly = 0; ly < ny_part; ly++) {
            result[y0 + ly] = dv[boff + local_offset(lx, ly, lz)];
        }
    }
    return result;
}

std::vector<float> BlockedFileReader::read_z_column(uint64_t x, uint64_t y) {
    if (x >= dim_x_ || y >= dim_y_)
        throw std::out_of_range("X or Y out of range");

    uint64_t bs = block_size_;
    const float* dv = mapped_file_.data();
    std::vector<float> result(dim_z_);

    uint32_t bx = static_cast<uint32_t>(x / bs);
    uint32_t by_ = static_cast<uint32_t>(y / bs);
    uint32_t lx = static_cast<uint32_t>(x % bs);
    uint32_t ly = static_cast<uint32_t>(y % bs);

    for (uint32_t bz = 0; bz < layout_.blocks_z; bz++) {
        uint64_t z0 = static_cast<uint64_t>(bz) * bs;
        uint64_t nz_part = std::min(bs, dim_z_ - z0);
        uint64_t boff = block_offset_float(bx, by_, bz);

        copy_z_run(dv, boff, lx, ly, 0, static_cast<uint32_t>(nz_part), &result[z0]);
    }
    return result;
}

// ── Batch column reads ───────────────────────────────────────────────

namespace {

template<typename F>
std::vector<std::vector<float>>
column_batch(const std::vector<std::pair<uint64_t, uint64_t>>& coords,
             int num_threads,
             F&& read_fn) {
    size_t n = coords.size();
    std::vector<std::vector<float>> results(n);

    int nt = num_threads;
    if (nt <= 0) nt = static_cast<int>(std::thread::hardware_concurrency());
    if (nt < 1) nt = 1;

    if (nt <= 1 || n <= 1) {
        for (size_t i = 0; i < n; i++)
            results[i] = read_fn(coords[i].first, coords[i].second);
        return results;
    }

    size_t nw = static_cast<size_t>(nt);
    if (nw > n) nw = n;
    std::vector<std::thread> threads;
    threads.reserve(nw);

    for (size_t w = 0; w < nw; w++) {
        threads.emplace_back([&, w, nw]() {
            for (size_t i = w; i < n; i += nw)
                results[i] = read_fn(coords[i].first, coords[i].second);
        });
    }
    for (auto& t : threads) t.join();
    return results;
}

} // anonymous namespace

std::vector<std::vector<float>>
BlockedFileReader::read_x_columns_batch(
    const std::vector<std::pair<uint64_t, uint64_t>>& coords,
    int num_threads) {
    return column_batch(coords, num_threads > 0 ? num_threads : num_threads_,
        [this](uint64_t a, uint64_t b) { return read_x_column(a, b); });
}

std::vector<std::vector<float>>
BlockedFileReader::read_y_columns_batch(
    const std::vector<std::pair<uint64_t, uint64_t>>& coords,
    int num_threads) {
    return column_batch(coords, num_threads > 0 ? num_threads : num_threads_,
        [this](uint64_t a, uint64_t b) { return read_y_column(a, b); });
}

std::vector<std::vector<float>>
BlockedFileReader::read_z_columns_batch(
    const std::vector<std::pair<uint64_t, uint64_t>>& coords,
    int num_threads) {
    return column_batch(coords, num_threads > 0 ? num_threads : num_threads_,
        [this](uint64_t a, uint64_t b) { return read_z_column(a, b); });
}

// ── Subvolume ────────────────────────────────────────────────────────

std::vector<float>
BlockedFileReader::read_subvolume(uint64_t xs, uint64_t xe,
                                   uint64_t ys, uint64_t ye,
                                   uint64_t zs, uint64_t ze) {
    if (xs >= xe || ys >= ye || zs >= ze)
        throw std::invalid_argument("Invalid subvolume range");
    if (xe > dim_x_ || ye > dim_y_ || ze > dim_z_)
        throw std::out_of_range("Subvolume exceeds dimensions");

    uint64_t nx = xe - xs;
    uint64_t ny = ye - ys;
    uint64_t nz = ze - zs;
    std::vector<float> result(nx * ny * nz);
    uint64_t bs = block_size_;
    const float* dv = mapped_file_.data();

    uint32_t bxs = static_cast<uint32_t>(xs / bs);
    uint32_t bxe = static_cast<uint32_t>((xe - 1) / bs) + 1;
    uint32_t bys = static_cast<uint32_t>(ys / bs);
    uint32_t bye = static_cast<uint32_t>((ye - 1) / bs) + 1;
    uint32_t bzs = static_cast<uint32_t>(zs / bs);
    uint32_t bze = static_cast<uint32_t>((ze - 1) / bs) + 1;

    for (uint32_t bx = bxs; bx < bxe; bx++) {
        for (uint32_t by_ = bys; by_ < bye; by_++) {
            for (uint32_t bz = bzs; bz < bze; bz++) {
                uint64_t x0 = static_cast<uint64_t>(bx) * bs;
                uint64_t y0 = static_cast<uint64_t>(by_) * bs;
                uint64_t z0 = static_cast<uint64_t>(bz) * bs;

                uint32_t lx_start = static_cast<uint32_t>(
                    (xs > x0) ? (xs - x0) : 0);
                uint32_t lx_end   = static_cast<uint32_t>(
                    std::min(xe - x0, bs));
                uint32_t ly_start = static_cast<uint32_t>(
                    (ys > y0) ? (ys - y0) : 0);
                uint32_t ly_end   = static_cast<uint32_t>(
                    std::min(ye - y0, bs));
                uint32_t lz_start = static_cast<uint32_t>(
                    (zs > z0) ? (zs - z0) : 0);
                uint32_t lz_end   = static_cast<uint32_t>(
                    std::min(ze - z0, bs));
                uint32_t ln = lz_end - lz_start;
                if (ln == 0) continue;

                uint64_t boff = block_offset_float(bx, by_, bz);

                for (uint32_t lx = lx_start; lx < lx_end; lx++) {
                    uint64_t dx = x0 + lx - xs;
                    for (uint32_t ly = ly_start; ly < ly_end; ly++) {
                        uint64_t dy = y0 + ly - ys;
                        uint64_t dst_idx = dx * ny * nz + dy * nz;
                        float* dst = &result[dst_idx + (z0 + lz_start - zs)];
                        copy_z_run(dv, boff, lx, ly, lz_start, lz_end, dst);
                    }
                }
            }
        }
    }
    return result;
}

std::vector<float> BlockedFileReader::read_full_volume() {
    return read_subvolume(0, dim_x_, 0, dim_y_, 0, dim_z_);
}

// ── Verify ───────────────────────────────────────────────────────────

float BlockedFileReader::read_point(uint64_t x, uint64_t y, uint64_t z) {
    uint64_t bs = block_size_;
    uint32_t bx = static_cast<uint32_t>(x / bs);
    uint32_t by_ = static_cast<uint32_t>(y / bs);
    uint32_t bz  = static_cast<uint32_t>(z / bs);
    uint32_t lx = static_cast<uint32_t>(x % bs);
    uint32_t ly = static_cast<uint32_t>(y % bs);
    uint32_t lz = static_cast<uint32_t>(z % bs);
    uint64_t boff = block_offset_float(bx, by_, bz);
    return mapped_file_.data()[boff + local_offset(lx, ly, lz)];
}

bool BlockedFileReader::verify(const std::string& raw_path,
                                uint64_t num_samples, float tol) {
    MappedFile raw(raw_path);
    const float* raw_data = raw.data();

    XorShift32 rng(42);

    std::vector<uint64_t> xs(num_samples), ys(num_samples), zs(num_samples);
    for (uint64_t s = 0; s < num_samples; s++) {
        xs[s] = rng.rand_u64_mod(dim_x_);
        ys[s] = rng.rand_u64_mod(dim_y_);
        zs[s] = rng.rand_u64_mod(dim_z_);
    }

    int nt = num_threads_;
    if (nt <= 1 || num_samples <= 1) nt = 1;

    size_t workers = static_cast<size_t>(nt);
    if (workers > num_samples) workers = num_samples;

    std::atomic<bool> all_passed{true};
    std::vector<std::thread> threads;
    threads.reserve(workers);

    for (size_t w = 0; w < workers; w++) {
        threads.emplace_back([&, w, workers]() {
            for (size_t s = w; s < num_samples; s += workers) {
                uint64_t x = xs[s], y = ys[s], z = zs[s];
                float blocked_val = read_point(x, y, z);
                uint64_t raw_idx = x * dim_y_ * dim_z_ + y * dim_z_ + z;
                float raw_val = raw_data[raw_idx];
                float abs_raw = std::abs(raw_val);
                float threshold = tol * std::max(abs_raw, 1e-10f);
                if (std::abs(blocked_val - raw_val) > threshold) {
                    all_passed.store(false, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    return all_passed.load(std::memory_order_relaxed);
}

} // namespace block3d
