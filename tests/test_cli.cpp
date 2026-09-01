// CLI integration / regression tests for block3d_cli.
//
// Root cause this guards against:
//   The `verify` subcommand used to take positional args in the order
//   <raw> <b3d>, while EVERY other subcommand (info/bench/extract/convert)
//   takes the .b3d file first. Users therefore naturally invoked:
//       block3d_cli verify <b3d> <raw> --samples N
//   which swapped the two files: BlockedFileReader opened the raw file as a
//   .b3d, the magic check threw std::runtime_error, and because neither
//   cmd_verify nor main caught exceptions, std::terminate() fired and on
//   Windows that surfaces as a fast-fail (0xC0000409 "stack buffer overrun")
//   with NO output -- i.e. an apparent crash, not a clean error.
//
// The fix:
//   1. verify now takes <b3d> <raw> (b3d first), consistent with all other
//      subcommands, so the natural invocation works.
//   2. main() wraps the subcommand dispatch in try/catch so any bad input
//      (wrong order / missing file / corrupt file) exits with code 2 and a
//      message instead of fast-failing.
//
// These tests drive the actual block3d_cli binary (built next to this test
// executable in the same CMake runtime output directory) so they exercise the
// real argument-parsing and exception-handling path, not just the library API.
//
// NOTE on command execution: we deliberately do NOT use std::system on Windows.
// std::system routes through `cmd.exe /c`, whose quote-stripping rule mangles
// command lines that contain several quoted paths (it strips the first and last
// quote, corrupting the exe path and a file path -> "filename syntax
// incorrect"). Instead we launch the child directly with CreateProcessW, which
// applies standard CommandLineToArgvW quoting and preserves every path. This
// also lets us observe the raw child exit code, including fast-fail crash codes
// such as 0xC0000409, so the test can prove the crash is gone.

#include <cstdlib>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdint>
#include <system_error>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

// Build a shell/argument-safe quoted path. We construct the command as a UTF-8
// std::string (via fs::path::u8string); on Windows it is converted to UTF-16
// for CreateProcessW, on POSIX it is handed to /bin/sh (UTF-8 native).
static std::string q(const fs::path& p) {
    return "\"" + p.u8string() + "\"";
}

// Run a command and return the child's exit code. On Windows this launches the
// child directly (no cmd.exe), so crash codes like 0xC0000409 are observable.
static int run_cmd(const std::string& cmd) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return -1;
    // Mutable, null-terminated buffer: CreateProcessW may modify the command
    // line in place, so it must not be const.
    std::vector<wchar_t> wcmd(static_cast<size_t>(wlen));
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, wcmd.data(), wlen);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE,
                             0, nullptr, nullptr, &si, &pi);
    if (!ok) return -1;

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
#else
    int rc = std::system(cmd.c_str());
    if (rc == -1) return -1;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return -1;
#endif
}

static int run_cmd_to_file(const std::string& cmd, const fs::path& out_path) {
#ifdef _WIN32
    int clen = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
    if (clen <= 0) return -1;
    std::vector<wchar_t> wcmd(static_cast<size_t>(clen));
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, wcmd.data(), clen);

    std::string out_utf8 = out_path.u8string();
    int plen = MultiByteToWideChar(CP_UTF8, 0, out_utf8.c_str(), -1, nullptr, 0);
    if (plen <= 0) return -1;
    std::vector<wchar_t> wpath(static_cast<size_t>(plen));
    MultiByteToWideChar(CP_UTF8, 0, out_utf8.c_str(), -1, wpath.data(), plen);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hOut = CreateFileW(wpath.data(), GENERIC_WRITE, FILE_SHARE_READ,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE) return -1;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hOut;
    si.hStdError = hOut;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                             0, nullptr, nullptr, &si, &pi);
    if (!ok) {
        CloseHandle(hOut);
        return -1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hOut);
    return static_cast<int>(code);
#else
    std::string full = cmd + " > " + q(out_path) + " 2>&1";
    return run_cmd(full);
#endif
}

// Locate block3d_cli next to this test executable (same build output dir),
// falling back to the current working directory.
static fs::path find_cli(const char* argv0) {
    std::string name = "block3d_cli";
#ifdef _WIN32
    name += ".exe";
#endif
    fs::path candidates[2] = {
        fs::path(argv0).parent_path() / name,
        fs::current_path() / name,
    };
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) return c;
    }
    return candidates[0];
}

#ifdef _WIN32
// 0xC0000409 == STATUS_STACK_BUFFER_OVERRUN, the fast-fail code that an
// uncaught C++ exception surfaces as on Windows. GetExitCodeProcess returns it
// verbatim when the child fast-fails.
static const int FAST_FAIL = static_cast<int>(0xC0000409u);
#else
static const int FAST_FAIL = 0;  // n/a on POSIX
#endif

int main(int argc, char* argv[]) {
    fs::path cli = find_cli(argv[0]);
    if (!fs::exists(cli)) {
        std::cerr << "Cannot locate block3d_cli at " << cli << "\n";
        return 1;
    }

    // Build a tiny dataset in a temp dir.
    fs::path tmp = fs::temp_directory_path() / "block3d_cli_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    fs::path raw = tmp / "in.dat";
    fs::path b3d = tmp / "out.b3d";
    fs::path b3d_micro = tmp / "out_micro.b3d";
    fs::path scrub = tmp / "cache scrub.bin";

    constexpr uint64_t dx = 16, dy = 20, dz = 24;
    constexpr uint64_t total = dx * dy * dz;
    {
        std::ofstream f(raw, std::ios::binary);
        for (uint64_t i = 0; i < total; i++) {
            float v = static_cast<float>(static_cast<long long>(i % 200) - 100);
            f.write(reinterpret_cast<const char*>(&v), sizeof(float));
        }
    }

    int failures = 0;
    auto expect_ok = [&](const char* label, const std::string& cmd) {
        int rc = run_cmd(cmd);
        std::cout << "  " << label << ": exit=" << rc;
        if (rc == 0) { std::cout << " PASS\n"; }
        else { std::cout << " FAIL (want 0)\n"; failures++; }
    };
    // A clean failure: non-zero, but NOT the fast-fail crash code.
    auto expect_clean_fail = [&](const char* label, const std::string& cmd) {
        int rc = run_cmd(cmd);
        std::cout << "  " << label << ": exit=" << rc;
        if (rc == 0) {
            std::cout << " FAIL (expected non-zero)\n"; failures++;
        } else if (rc == FAST_FAIL) {
            std::cout << " FAIL (crash 0xC0000409 -- uncaught exception)\n"; failures++;
        } else {
            std::cout << " PASS (clean failure)\n";
        }
    };
    auto expect_output_contains = [&](const char* label, const std::string& cmd,
                                      const std::string& needle) {
        fs::path out = tmp / (std::string("cmd_output_") + std::to_string(failures) + ".txt");
        int rc = run_cmd_to_file(cmd, out);
        std::ifstream f(out);
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string text = ss.str();
        std::cout << "  " << label << ": exit=" << rc;
        if (rc == 0 && text.find(needle) != std::string::npos) {
            std::cout << " PASS\n";
        } else {
            std::cout << " FAIL (want exit 0 and output containing '" << needle << "')\n";
            std::cout << text << "\n";
            failures++;
        }
    };

    std::cout << "block3d CLI tests\n";
    std::cout << "=================\n";
    std::cout << "CLI: " << cli.string() << "\n";

    // 1. convert raw -> b3d (output is 2nd positional; sanity baseline).
    expect_ok("convert <raw> <b3d>",
              q(cli) + " convert " + q(raw) + " " + q(b3d) +
              " --dim-x 16 --dim-y 20 --dim-z 24 --block-size 16 --threads 2 --no-progress");

    expect_output_contains("info legacy layout",
        q(cli) + " info " + q(b3d),
        "Layout:        legacy");

    expect_ok("convert micro-tiled v2",
              q(cli) + " convert " + q(raw) + " " + q(b3d_micro) +
              " --dim-x 16 --dim-y 20 --dim-z 24 --block-size 16 --threads 2"
              " --layout micro-tiled --micro-size 8 --no-progress");
    expect_ok("verify micro-tiled v2",
              q(cli) + " verify " + q(b3d_micro) + " " + q(raw) + " --samples 500");
    expect_output_contains("info micro layout",
        q(cli) + " info " + q(b3d_micro),
        "Layout:        micro-tiled");
    expect_output_contains("info micro version",
        q(cli) + " info " + q(b3d_micro),
        "Version:       2");
    expect_output_contains("info micro size",
        q(cli) + " info " + q(b3d_micro),
        "Micro size:    8");
    expect_clean_fail("convert bad micro size 0",
        q(cli) + " convert " + q(raw) + " " + q(tmp / "bad0.b3d") +
        " --dim-x 16 --dim-y 20 --dim-z 24 --block-size 16 --layout micro-tiled --micro-size 0 --no-progress");
    expect_clean_fail("convert bad micro size 7",
        q(cli) + " convert " + q(raw) + " " + q(tmp / "bad7.b3d") +
        " --dim-x 16 --dim-y 20 --dim-z 24 --block-size 16 --layout micro-tiled --micro-size 7 --no-progress");
    expect_clean_fail("convert non-divisible micro block",
        q(cli) + " convert " + q(raw) + " " + q(tmp / "bad_bs.b3d") +
        " --dim-x 16 --dim-y 20 --dim-z 24 --block-size 20 --layout micro-tiled --micro-size 8 --no-progress");

    // 2. verify with the canonical order <b3d> <raw> (b3d first).
    //    Regression for the arg-order bug: this is the natural invocation that
    //    previously swapped the files and fast-failed with 0xC0000409.
    expect_ok("verify <b3d> <raw>",
              q(cli) + " verify " + q(b3d) + " " + q(raw) + " --samples 500");

    // 3. verify with the OLD/swapped order <raw> <b3d>: the raw file is now
    //    opened as a .b3d -> magic mismatch -> exception. This is the EXACT
    //    scenario that used to crash with 0xC0000409; it must now exit cleanly
    //    with a non-zero code (caught by main's try/catch).
    expect_clean_fail("verify <raw> <b3d> (bad order, no crash)",
                      q(cli) + " verify " + q(raw) + " " + q(b3d) + " --samples 500");

    // 4. verify against a non-existent raw file -> clean failure, no crash.
    fs::path nope = tmp / "does_not_exist.dat";
    expect_clean_fail("verify missing raw (no crash)",
                      q(cli) + " verify " + q(b3d) + " " + q(nope) + " --samples 10");

    // 5. verify with a non-existent b3d -> clean failure, no crash.
    expect_clean_fail("verify missing b3d (no crash)",
                      q(cli) + " verify " + q(nope) + " " + q(raw) + " --samples 10");

    // 6. cache-prepare creates a reusable non-empty scrub file, including paths
    //    with spaces, without making bench create it implicitly.
    expect_ok("cache-prepare small scrub",
              q(cli) + " cache-prepare " + q(scrub) + " --size 1MB");
    if (!fs::exists(scrub) || fs::file_size(scrub) != 1024 * 1024) {
        std::cout << "  cache-prepare file size: FAIL\n";
        failures++;
    }

    // 7. Default both-mode can be exercised on tiny data with --cold-method none;
    //    it must emit cold/hot phases and matching plan hashes without requiring
    //    unstable timing assertions.
    expect_output_contains("bench both cold-hot",
        q(cli) + " bench " + q(b3d)
        + " --num-reads 2 --threads 2 --cache-mode both --cold-method none --warmup-scope workload",
        "BENCHMARK_COMPARE");

    // 8. Phase 1 fused batch mode is logged in machine-readable form.
    expect_output_contains("bench fused config logs",
        q(cli) + " bench " + q(b3d)
        + " --num-reads 1 --threads 2 --cache-mode hot --warmup-scope workload"
        + " --batch-read fused --batch-window 3 --pipeline off --read-dispatch round-robin",
        "BATCH_READ mode=fused window_slices=3");

    // 9. Compatibility --warm-up means hot-only, and invalid/conflicting cache
    //    parameters fail cleanly instead of running the wrong benchmark state.
    expect_output_contains("bench warm-up hot-only",
        q(cli) + " bench " + q(b3d)
        + " --num-reads 1 --threads 2 --warm-up --warmup-scope workload",
        "CACHE_PHASE name=hot_prefetched");
    expect_clean_fail("bench invalid cache-mode",
        q(cli) + " bench " + q(b3d) + " --cache-mode tepid --cold-method none");
    expect_clean_fail("bench warm-up conflict",
        q(cli) + " bench " + q(b3d) + " --warm-up --cache-mode both --cold-method none");
    expect_clean_fail("bench missing scrub file",
        q(cli) + " bench " + q(b3d) + " --cache-mode cold --cold-method scrub --cold-scrub-file " + q(nope));
    expect_clean_fail("bench invalid batch-window",
        q(cli) + " bench " + q(b3d) + " --cache-mode hot --batch-window 0");
    expect_clean_fail("bench read-only rejects pipeline switch",
        q(cli) + " bench " + q(b3d) + " --cache-mode hot --pipeline on");
    expect_output_contains("bench contiguous dispatch switch",
        q(cli) + " bench " + q(b3d) + " --num-reads 1 --threads 2 --cache-mode hot --read-dispatch contiguous",
        "READ_DISPATCH strategy=contiguous");

    fs::remove_all(tmp, ec);

    std::cout << "=================\n";
    if (failures > 0) {
        std::cout << failures << " CLI test(s) FAILED!\n";
        return 1;
    }
    std::cout << "All CLI tests PASSED\n";
    return 0;
}
