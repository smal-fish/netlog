#include "netlog/parser.h"
#include "netlog/stats.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

struct ReadSummary {
    std::uint64_t read_lines = 0;
    std::uint64_t valid_records = 0;
    std::uint64_t invalid_records = 0;

    void merge(const ReadSummary& other) {
        read_lines += other.read_lines;
        valid_records += other.valid_records;
        invalid_records += other.invalid_records;
    }
};

void print_usage(std::ostream& out) {
    out << "Usage: netlog --input <file-or-directory> [--threads N]\n"
        << "       netlog --help\n";
}

void print_summary(const ReadSummary& summary) {
    std::cout << "read_lines=" << summary.read_lines << '\n'
              << "valid_records=" << summary.valid_records << '\n'
              << "invalid_records=" << summary.invalid_records << '\n';
}

void print_report(std::ostream& out, const netlog::Stats& stats) {
    out << "devices:\n"
        << "device_id requests errors error_rate avg_us max_us\n";
    out << std::fixed << std::setprecision(2);
    for (const auto& [id, device] : stats.devices()) {
        const auto error_rate = 100.0L * device.errors / device.requests;
        const auto average_us = static_cast<long double>(device.total_duration_us)
                                / device.requests;
        out << id << ' ' << device.requests << ' ' << device.errors << ' '
            << error_rate << "% " << average_us << ' '
            << device.max_duration_us << '\n';
    }

    out << "top_events (up to 3):\n";
    for (const auto& [event, count] : stats.top_events(3)) {
        out << event << ' ' << count << '\n';
    }
}

namespace fs = std::filesystem;

std::vector<fs::path> collect_files(const fs::path& input) {
    const auto status = fs::status(input);
    if (fs::is_regular_file(status)) {
        return {input};
    }
    if (!fs::is_directory(status)) {
        std::cerr << "Cannot inspect input file or directory: " << input.string() << '\n';
        return {};
    }
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(input)) {
        if (entry.path().extension() == ".log" && fs::is_regular_file(entry.symlink_status())) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::cerr << "No .log files in directory: " << input.string() << '\n';
    }
    return files;
}

enum class FileStatus { Ok, OpenFailed, ReadFailed };

FileStatus analyze_file(const fs::path& path, ReadSummary& summary, netlog::Stats& stats,
                        std::ostream& diagnostics) {
    // binary 保留原始字节；getline 按 LF 分行，解析器接受行末 CR。
    // ifstream 在离开作用域时自动关闭文件（RAII）。
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        diagnostics << "Cannot open input file: " << path.string() << '\n';
        return FileStatus::OpenFailed;
    }

    std::uint64_t line_number = 0;
    std::string line;
    while (std::getline(input, line)) {
        ++line_number;
        ++summary.read_lines;
        const auto result = netlog::parse_line(line);
        if (const auto* error = std::get_if<netlog::ParseError>(&result)) {
            ++summary.invalid_records;
            diagnostics << path.string() << ':' << line_number << ": "
                        << netlog::error_message(*error) << '\n';
            continue;
        }
        stats.add(std::get<netlog::Record>(result));
        ++summary.valid_records;
    }

    if (input.bad() || (input.fail() && !input.eof())) {
        diagnostics << "Failed while reading input file: " << path.string()
                    << " (summary is partial)\n";
        return FileStatus::ReadFailed;
    }
    return FileStatus::Ok;
}

FileStatus analyze_serial(const std::vector<fs::path>& files, ReadSummary& summary,
                          netlog::Stats& stats) {
    for (const auto& path : files) {
        const auto status = analyze_file(path, summary, stats, std::cerr);
        if (status != FileStatus::Ok) {
            return status;
        }
    }
    return FileStatus::Ok;
}

FileStatus analyze_parallel(const std::vector<fs::path>& files, std::size_t worker_count,
                            ReadSummary& summary, netlog::Stats& stats) {
    struct FileResult {
        ReadSummary summary;
        std::ostringstream diagnostics;
        FileStatus status = FileStatus::Ok;
        std::exception_ptr exception;
    };
    // 每个线程独占一张统计表，每个文件独占一个结果槽。
    std::vector<netlog::Stats> local_stats(worker_count);
    std::vector<FileResult> results(files.size());
    std::atomic<std::size_t> next_file{0};

    const auto work = [&](std::size_t worker_id) {
        auto& local = local_stats[worker_id];
        for (;;) {
            const auto index = next_file.fetch_add(1, std::memory_order_relaxed);
            if (index >= files.size()) {
                return;
            }
            auto& result = results[index];
            try {
                result.status = analyze_file(files[index], result.summary, local, result.diagnostics);
                if (result.status == FileStatus::Ok) {
                    continue;
                }
            } catch (...) {
                result.exception = std::current_exception();
            }
            // 已领到的文件继续读，两种失败均停止分发新文件。
            next_file.store(files.size(), std::memory_order_relaxed);
            return;
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    std::exception_ptr startup_failure;
    try {
        for (std::size_t id = 0; id < worker_count; ++id) {
            workers.emplace_back(work, id);
        }
    } catch (...) {
        next_file.store(files.size(), std::memory_order_relaxed);
        startup_failure = std::current_exception();
    }
    for (auto& worker : workers) {
        worker.join();
    }
    if (startup_failure) {
        std::rethrow_exception(startup_failure);
    }

    // 等全部线程结束后，按文件顺序输出诊断和汇总读入行数。
    for (const auto& result : results) {
        const auto diagnostics = result.diagnostics.str();
        if (!diagnostics.empty()) {
            std::cerr << diagnostics;
        }
        if (result.exception) {
            std::rethrow_exception(result.exception);
        }
        summary.merge(result.summary);
        if (result.status != FileStatus::Ok) {
            return result.status;
        }
    }
    for (const auto& local : local_stats) {
        stats.merge(local);
    }
    return FileStatus::Ok;
}

int analyze_input(const fs::path& input, std::size_t requested_threads) {
    std::vector<fs::path> files;
    try {
        files = collect_files(input);
    } catch (const fs::filesystem_error& error) {
        std::cerr << "Cannot inspect input file or directory: " << error.what() << '\n';
        return 3;
    }
    if (files.empty()) {
        return 3;
    }

    ReadSummary summary;
    netlog::Stats stats;
    const auto worker_count = std::min(requested_threads, files.size());
    const auto status = worker_count == 1 ? analyze_serial(files, summary, stats)
                                          : analyze_parallel(files, worker_count, summary, stats);
    if (status == FileStatus::OpenFailed) {
        return 3;
    }
    print_summary(summary);
    if (status == FileStatus::ReadFailed) {
        return 3;
    }
    print_report(std::cout, stats);
    return 0;
}

bool parse_thread_count(std::string_view text, std::size_t& count) {
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, count);
    return result.ec == std::errc{} && result.ptr == end && count > 0;
}

int run(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_usage(std::cout);
        return 0;
    }
    std::string_view input;
    std::size_t threads = 1;
    bool has_threads = false;
    int i = 1;
    for (; i + 1 < argc; i += 2) {
        const std::string_view option = argv[i];
        const std::string_view value = argv[i + 1];
        if (option == "--input" && input.empty() && !value.empty()) {
            input = value;
        } else if (option == "--threads" && !has_threads && parse_thread_count(value, threads)) {
            has_threads = true;
        } else {
            break;
        }
    }
    if (i != argc || input.empty()) {
        std::cerr << "Invalid command line.\n";
        print_usage(std::cerr);
        return 2;
    }
    return analyze_input(fs::path(input), threads);
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        // 格式错误通过返回值处理；这里只兜底非预期运行错误。
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 4;
    }
}
