#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <list>
#include <mutex>
#include <utility>
#include <future>
#include <atomic>
#include <functional>
#include <memory>
#include "core.hpp"
#include "rng.hpp"

namespace block3d {

class MappedFile {
public:
    MappedFile() = default;
    MappedFile(const std::string& path);
    ~MappedFile();

    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    const float* data() const {
        return reinterpret_cast<const float*>(mapped_data_);
    }
    const uint8_t* bytes() const {
        return reinterpret_cast<const uint8_t*>(mapped_data_);
    }
    size_t size() const { return size_; }
    bool is_open() const { return mapped_data_ != nullptr; }
    void close();

    void prefault(size_t offset, size_t length, size_t stride = 4096);

private:
    void*  mapped_data_ = nullptr;
    size_t size_        = 0;

#ifdef _WIN32
    void*  file_handle_ = nullptr;
    void*  map_handle_  = nullptr;
#else
    int    fd_          = -1;
#endif

    void do_mmap(const std::string& path);
};

struct BlockCoord {
    uint32_t bx, by_, bz;
};

enum class ReadDispatchStrategy {
    RoundRobin,
    Contiguous,
};

struct SliceBatchOptions {
    int num_threads = 0;
    size_t window_slices = 1;
};

using SliceConsumer = std::function<void(
    size_t request_pos,
    uint64_t index,
    std::vector<float>&& slice)>;

class BlockedFileReader {
public:
    BlockedFileReader(const std::string& file_path,
                      int num_threads = 0,
                      uint64_t max_memory_mb = 0,
                      ReadDispatchStrategy dispatch_strategy = ReadDispatchStrategy::RoundRobin);
    ~BlockedFileReader();

    BlockedFileReader(const BlockedFileReader&) = delete;
    BlockedFileReader& operator=(const BlockedFileReader&) = delete;
    BlockedFileReader(BlockedFileReader&&) = delete;
    BlockedFileReader& operator=(BlockedFileReader&&) = delete;

    std::vector<float> read_x_slice(uint64_t x);
    std::vector<float> read_y_slice(uint64_t y);
    std::vector<float> read_z_slice(uint64_t z);
    std::vector<float> read_slice(char axis, uint64_t index);

    std::vector<std::vector<float>>
    read_slices_batch(char axis, const std::vector<uint64_t>& indices,
                      int num_threads = 0);
    void read_slices_batch_stream(char axis,
                                  const std::vector<uint64_t>& indices,
                                  const SliceBatchOptions& options,
                                  const SliceConsumer& consumer);

    std::vector<float> read_x_column(uint64_t y, uint64_t z);
    std::vector<float> read_y_column(uint64_t x, uint64_t z);
    std::vector<float> read_z_column(uint64_t x, uint64_t y);

    std::vector<std::vector<float>>
    read_x_columns_batch(const std::vector<std::pair<uint64_t, uint64_t>>& coords,
                         int num_threads = 0);
    std::vector<std::vector<float>>
    read_y_columns_batch(const std::vector<std::pair<uint64_t, uint64_t>>& coords,
                         int num_threads = 0);
    std::vector<std::vector<float>>
    read_z_columns_batch(const std::vector<std::pair<uint64_t, uint64_t>>& coords,
                         int num_threads = 0);

    std::vector<float> read_subvolume(uint64_t xs, uint64_t xe,
                                      uint64_t ys, uint64_t ye,
                                      uint64_t zs, uint64_t ze);

    std::vector<float> read_full_volume();

    float read_point(uint64_t x, uint64_t y, uint64_t z);

    bool verify(const std::string& raw_path,
                uint64_t num_samples = 1000, float tol = 1e-3f);

    void warm_up(bool async = true,
                 size_t stride = 4096,
                 uint64_t max_memory_mb = 0);
    bool is_warm_up_done() const;
    void wait_warm_up();

    uint64_t     dim_x() const { return dim_x_; }
    uint64_t     dim_y() const { return dim_y_; }
    uint64_t     dim_z() const { return dim_z_; }
    uint64_t     block_size() const { return block_size_; }
    uint64_t     total_blocks() const { return total_blocks_; }
    uint64_t     data_offset() const { return data_offset_; }
    int          num_threads() const { return num_threads_; }
    uint64_t     max_memory_mb() const { return max_memory_mb_; }
    size_t       thread_pool_workers() const;
    uint64_t     thread_pool_jobs() const;
    uint64_t     thread_pool_serial_fallbacks() const;
    ReadDispatchStrategy read_dispatch_strategy() const { return dispatch_strategy_; }
    uint32_t version() const { return version_; }
    BlockInnerLayout inner_layout() const { return inner_layout_; }
    uint32_t micro_size() const { return micro_size_; }
    const BlockLayout3D& layout() const { return layout_; }

private:
    uint64_t dim_x_ = 0, dim_y_ = 0, dim_z_ = 0;
    uint64_t block_size_ = 0;
    uint64_t total_blocks_ = 0;
    uint64_t data_offset_ = 0;
    uint32_t version_ = VERSION_LEGACY;
    BlockInnerLayout inner_layout_ = BlockInnerLayout::LegacyXYZ;
    uint32_t micro_size_ = 0;
    int num_threads_ = 1;
    uint64_t max_memory_mb_ = 0;
    ReadDispatchStrategy dispatch_strategy_ = ReadDispatchStrategy::RoundRobin;

    BlockLayout3D   layout_;
    MappedFile      mapped_file_;
    std::vector<uint64_t> block_offsets_;

    static constexpr size_t CACHE_MAX_ENTRIES = 256;

    struct CacheKey {
        char axis; uint32_t block_idx;
        bool operator==(const CacheKey& o) const {
            return axis == o.axis && block_idx == o.block_idx;
        }
    };
    struct KeyEqual {
        bool operator()(const CacheKey& a, const CacheKey& b) const {
            return a.axis == b.axis && a.block_idx == b.block_idx;
        }
    };
    struct KeyHash {
        size_t operator()(const CacheKey& k) const {
            return static_cast<size_t>(k.axis) * 31
                 + static_cast<size_t>(k.block_idx);
        }
    };

    std::unordered_map<CacheKey, std::vector<BlockCoord>, KeyHash, KeyEqual> cache_;
    std::list<CacheKey> cache_order_;
    std::mutex cache_mutex_;

    class ThreadPool;
    std::unique_ptr<ThreadPool> thread_pool_;

    std::future<void> warm_up_future_;
    std::atomic<bool> warm_up_done_{false};

    void cache_prune();

    uint64_t block_offset_float(uint32_t bx, uint32_t by_, uint32_t bz) const;
    uint64_t local_offset(uint32_t lx, uint32_t ly, uint32_t lz) const;
    void copy_z_run(const float* data, uint64_t block_float_offset,
                    uint32_t lx, uint32_t ly,
                    uint32_t lz_start, uint32_t lz_end,
                    float* dst) const;
    // Returns a *copy* of the sorted block list. Must NOT return a reference
    // into the cache: the cache_mutex_ is released before the caller uses the
    // result, so an internal reference could dangle after a rehash/prune or a
    // concurrent overwrite of the same key.
    std::vector<BlockCoord> sorted_block_list(char axis, uint64_t index);

    template<typename F>
    void for_each_block_parallel(int requested_threads,
                                 const std::vector<BlockCoord>& blocks,
                                 F&& process);
    template<typename F>
    void for_each_block_parallel(const std::vector<BlockCoord>& blocks, F&& process);

};

} // namespace block3d
