#include "benchmark_cache/benchmark_cache.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace block3d {

const char* to_string(CacheMode mode) {
    switch (mode) {
    case CacheMode::Cold: return "cold";
    case CacheMode::Hot:  return "hot";
    case CacheMode::Both: return "both";
    }
    return "unknown";
}

const char* to_string(ColdMethod method) {
    switch (method) {
    case ColdMethod::Scrub: return "scrub";
    case ColdMethod::None:  return "none";
    }
    return "unknown";
}

const char* to_string(ColdIsolation isolation) {
    switch (isolation) {
    case ColdIsolation::Suite: return "suite";
    case ColdIsolation::Case:  return "case";
    }
    return "unknown";
}

const char* to_string(WarmupScope scope) {
    switch (scope) {
    case WarmupScope::Dataset:  return "dataset";
    case WarmupScope::Workload: return "workload";
    }
    return "unknown";
}

bool parse_cache_mode(const std::string& text, CacheMode& out) {
    if (text == "cold") { out = CacheMode::Cold; return true; }
    if (text == "hot")  { out = CacheMode::Hot;  return true; }
    if (text == "both") { out = CacheMode::Both; return true; }
    return false;
}

bool parse_cold_method(const std::string& text, ColdMethod& out) {
    if (text == "scrub") { out = ColdMethod::Scrub; return true; }
    if (text == "none")  { out = ColdMethod::None;  return true; }
    return false;
}

bool parse_cold_isolation(const std::string& text, ColdIsolation& out) {
    if (text == "suite") { out = ColdIsolation::Suite; return true; }
    if (text == "case")  { out = ColdIsolation::Case;  return true; }
    return false;
}

bool parse_warmup_scope(const std::string& text, WarmupScope& out) {
    if (text == "dataset")  { out = WarmupScope::Dataset;  return true; }
    if (text == "workload") { out = WarmupScope::Workload; return true; }
    return false;
}

bool cache_mode_has_cold(CacheMode mode) {
    return mode == CacheMode::Cold || mode == CacheMode::Both;
}

bool cache_mode_has_hot(CacheMode mode) {
    return mode == CacheMode::Hot || mode == CacheMode::Both;
}

size_t query_system_page_size() {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<size_t>(si.dwPageSize ? si.dwPageSize : 4096);
#else
    long ps = ::sysconf(_SC_PAGESIZE);
    return ps > 0 ? static_cast<size_t>(ps) : 4096;
#endif
}

uint64_t query_physical_memory_bytes() {
#ifdef _WIN32
    MEMORYSTATUSEX st{};
    st.dwLength = sizeof(st);
    if (GlobalMemoryStatusEx(&st)) return static_cast<uint64_t>(st.ullTotalPhys);
    return 0;
#else
    long pages = ::sysconf(_SC_PHYS_PAGES);
    long page_size = ::sysconf(_SC_PAGESIZE);
    if (pages <= 0 || page_size <= 0) return 0;
    return static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
#endif
}

uint64_t required_scrub_bytes(double ram_ratio) {
    if (ram_ratio <= 0.0) return 0;
    uint64_t ram = query_physical_memory_bytes();
    long double bytes = static_cast<long double>(ram) * ram_ratio;
    if (bytes > static_cast<long double>(std::numeric_limits<uint64_t>::max()))
        return std::numeric_limits<uint64_t>::max();
    return static_cast<uint64_t>(bytes);
}

uint64_t parse_size_bytes(const std::string& text, bool& ok) {
    ok = false;
    if (text.empty()) return 0;
    size_t end = 0;
    double value = 0.0;
    try {
        value = std::stod(text, &end);
    } catch (...) {
        return 0;
    }
    if (value < 0.0) return 0;
    std::string suffix = text.substr(end);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    double scale = 1.0;
    if (suffix.empty() || suffix == "b") scale = 1.0;
    else if (suffix == "k" || suffix == "kb" || suffix == "kib") scale = 1024.0;
    else if (suffix == "m" || suffix == "mb" || suffix == "mib") scale = 1024.0 * 1024.0;
    else if (suffix == "g" || suffix == "gb" || suffix == "gib") scale = 1024.0 * 1024.0 * 1024.0;
    else return 0;

    long double bytes = static_cast<long double>(value) * scale;
    if (bytes > static_cast<long double>(std::numeric_limits<uint64_t>::max())) return 0;
    ok = true;
    return static_cast<uint64_t>(bytes);
}

uint64_t fnv1a64_update(uint64_t hash, const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++) {
        hash ^= static_cast<uint64_t>(p[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t fnv1a64_u64(uint64_t hash, uint64_t value) {
    return fnv1a64_update(hash, &value, sizeof(value));
}

namespace {

uint64_t splitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

uint64_t touch_file_pages(const fs::path& path, uint64_t passes,
                          uint64_t& bytes_read, double& elapsed_sec) {
    const size_t page_size = query_system_page_size();
    std::vector<char> buffer(4 * 1024 * 1024);
    if (buffer.size() < page_size) buffer.resize(page_size);
    uint64_t checksum = 0;
    bytes_read = 0;

    auto t0 = std::chrono::high_resolution_clock::now();
    for (uint64_t pass = 0; pass < passes; pass++) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("cannot open scrub file");
        uint64_t absolute = 0;
        char last_byte = 0;
        bool saw_byte = false;
        for (;;) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            std::streamsize got = in.gcount();
            if (got <= 0) break;
            saw_byte = true;
            last_byte = buffer[static_cast<size_t>(got - 1)];
            for (std::streamsize pos = 0; pos < got; pos += static_cast<std::streamsize>(page_size)) {
                checksum += static_cast<unsigned char>(buffer[static_cast<size_t>(pos)]);
            }
            absolute += static_cast<uint64_t>(got);
            bytes_read += static_cast<uint64_t>(got);
        }
        if (!in.eof()) throw std::runtime_error("error while reading scrub file");
        if (saw_byte) checksum += static_cast<unsigned char>(last_byte);
        (void)absolute;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    elapsed_sec = std::chrono::duration<double>(t1 - t0).count();
    return checksum;
}

} // namespace

CachePrepareResult prepare_cache_scrub_file(const CachePrepareOptions& options) {
    CachePrepareResult result;
    result.path = options.path;
    result.bytes = options.size_bytes ? options.size_bytes
                                      : required_scrub_bytes(options.cold_scrub_ratio);
    if (result.path.empty()) {
        result.message = "missing scrub file path";
        return result;
    }
    if (result.bytes == 0) {
        result.message = "scrub size is zero";
        return result;
    }

    fs::path path(result.path);
    std::error_code ec;
    if (fs::exists(path, ec) && !options.overwrite) {
        uint64_t existing = fs::file_size(path, ec);
        if (!ec && existing >= result.bytes) {
            result.ok = true;
            result.reused = true;
            result.bytes = existing;
            result.message = "existing file reused";
            return result;
        }
        result.message = "file exists but is smaller than requested; pass --overwrite to replace it";
        return result;
    }

    fs::path parent = path.parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    if (ec) {
        result.message = "cannot create parent directory: " + ec.message();
        return result;
    }

    fs::path tmp = path;
    tmp += ".tmp";
    fs::remove(tmp, ec);

    auto t0 = std::chrono::high_resolution_clock::now();
    uint64_t checksum = 0;
    try {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot create temp scrub file");
        std::vector<uint64_t> words(1024 * 1024 / sizeof(uint64_t));
        uint64_t written = 0;
        uint64_t state = 0xB10C3D5C2A4E9F17ULL;
        while (written < result.bytes) {
            for (auto& word : words) {
                word = splitmix64(state);
                checksum ^= word;
            }
            uint64_t remaining = result.bytes - written;
            size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, words.size() * sizeof(uint64_t)));
            out.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(chunk));
            if (!out) throw std::runtime_error("write failed while creating scrub file");
            written += chunk;
        }
        out.close();
        if (!out) throw std::runtime_error("close failed while creating scrub file");
        fs::rename(tmp, path, ec);
        if (ec) {
            fs::remove(path, ec);
            ec.clear();
            fs::rename(tmp, path, ec);
            if (ec) throw std::runtime_error("cannot move temp scrub file into place: " + ec.message());
        }
    } catch (const std::exception& e) {
        fs::remove(tmp, ec);
        result.message = e.what();
        return result;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    result.elapsed_sec = std::chrono::duration<double>(t1 - t0).count();
    result.checksum = checksum;
    result.ok = true;
    result.message = "created";
    return result;
}

CacheScrubResult run_cache_scrub(const CacheOptions& options) {
    CacheScrubResult result;
    result.path = options.cold_scrub_file;
    result.required_bytes = required_scrub_bytes(options.cold_scrub_ratio);
    result.passes = options.cold_scrub_passes;
    result.ram_ratio = options.cold_scrub_ratio;

    if (options.cold_method == ColdMethod::None) {
        result.ok = true;
        result.message = "cold_first_touch";
        return result;
    }
    if (result.path.empty()) {
        result.message = "--cold-scrub-file is required for --cold-method scrub";
        return result;
    }
    if (result.passes == 0) {
        result.message = "--cold-scrub-passes must be > 0";
        return result;
    }

    std::error_code ec;
    fs::path path(result.path);
    if (!fs::exists(path, ec)) {
        result.message = "scrub file does not exist";
        return result;
    }
    result.file_bytes = fs::file_size(path, ec);
    if (ec) {
        result.message = "cannot stat scrub file: " + ec.message();
        return result;
    }
    if (result.file_bytes == 0) {
        result.message = "scrub file is empty";
        return result;
    }
    if (result.required_bytes > 0 && result.file_bytes < result.required_bytes) {
        result.message = "scrub file is smaller than configured RAM ratio requirement";
        return result;
    }

    try {
        result.checksum = touch_file_pages(path, result.passes,
                                           result.bytes_read,
                                           result.elapsed_sec);
    } catch (const std::exception& e) {
        result.message = e.what();
        return result;
    }

    if (result.bytes_read != result.file_bytes * result.passes) {
        result.message = "scrub short read";
        return result;
    }
    if (options.cold_settle_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(options.cold_settle_ms));
    }
    result.ok = true;
    result.message = "scrub_success";
    return result;
}

} // namespace block3d
