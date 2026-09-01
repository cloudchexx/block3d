#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace block3d {

enum class CacheMode { Cold, Hot, Both };
enum class ColdMethod { Scrub, None };
enum class ColdIsolation { Suite, Case };
enum class WarmupScope { Dataset, Workload };

struct CacheOptions {
    CacheMode mode = CacheMode::Both;
    ColdMethod cold_method = ColdMethod::Scrub;
    ColdIsolation cold_isolation = ColdIsolation::Suite;
    WarmupScope warmup_scope = WarmupScope::Workload;
    std::string cold_scrub_file;
    double cold_scrub_ratio = 1.50;
    uint32_t cold_scrub_passes = 1;
    uint32_t cold_settle_ms = 1000;
};

struct CachePrepareOptions {
    std::string path;
    uint64_t size_bytes = 0;
    double cold_scrub_ratio = 1.50;
    bool overwrite = false;
};

struct CachePrepareResult {
    bool ok = false;
    bool reused = false;
    std::string path;
    uint64_t bytes = 0;
    uint64_t checksum = 0;
    double elapsed_sec = 0.0;
    std::string message;
};

struct CacheScrubResult {
    bool ok = false;
    std::string path;
    uint64_t required_bytes = 0;
    uint64_t file_bytes = 0;
    uint64_t bytes_read = 0;
    uint64_t checksum = 0;
    uint32_t passes = 0;
    double ram_ratio = 0.0;
    double elapsed_sec = 0.0;
    std::string message;
};

const char* to_string(CacheMode mode);
const char* to_string(ColdMethod method);
const char* to_string(ColdIsolation isolation);
const char* to_string(WarmupScope scope);

bool parse_cache_mode(const std::string& text, CacheMode& out);
bool parse_cold_method(const std::string& text, ColdMethod& out);
bool parse_cold_isolation(const std::string& text, ColdIsolation& out);
bool parse_warmup_scope(const std::string& text, WarmupScope& out);

bool cache_mode_has_cold(CacheMode mode);
bool cache_mode_has_hot(CacheMode mode);

size_t query_system_page_size();
uint64_t query_physical_memory_bytes();
uint64_t required_scrub_bytes(double ram_ratio);
uint64_t parse_size_bytes(const std::string& text, bool& ok);

uint64_t fnv1a64_update(uint64_t hash, const void* data, size_t size);
uint64_t fnv1a64_u64(uint64_t hash, uint64_t value);

CachePrepareResult prepare_cache_scrub_file(const CachePrepareOptions& options);
CacheScrubResult run_cache_scrub(const CacheOptions& options);

} // namespace block3d
