#include "../include/netlog/parser.h"
#include "../include/netlog/stats.h"

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
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

struct Filter {
    std::string_view device;
    std::optional<std::int64_t> since;
    std::optional<std::int64_t> until;

    bool active() const {
        return !device.empty() || since.has_value() || until.has_value();
    }

    bool matches(const netlog::Record& record) const {
        return (device.empty() || record.device_id == device)
            && (!since || record.timestamp >= *since)
            && (!until || record.timestamp < *until);
    }
};

struct AnalysisOptions {
    std::size_t threads = 1;
    std::size_t top_k = 3;
    std::int64_t bucket_seconds = 0;
    Filter filter;
};

struct ReadSummary {
    std::uint64_t read_lines = 0;
    std::uint64_t valid_records = 0;
    std::uint64_t matched_records = 0;
    std::uint64_t invalid_records = 0;

    void merge(const ReadSummary& other) {
        read_lines += other.read_lines;
        valid_records += other.valid_records;
        matched_records += other.matched_records;
        invalid_records += other.invalid_records;
    }
};

void print_usage(std::ostream& out) {
    out << "Usage: netlog --input <file-or-directory> [--threads N] [--top K]\n"
        << "       [--device ID] [--since UNIX_SEC] [--until UNIX_SEC] [--bucket SECONDS]\n"
        << "       netlog --help\n";
}

void print_summary(const ReadSummary& summary, bool show_matched) {
    std::cout << "read_lines=" << summary.read_lines << '\n'
              << "valid_records=" << summary.valid_records << '\n'
              << "invalid_records=" << summary.invalid_records << '\n';
    if (show_matched) {
        std::cout << "matched_records=" << summary.matched_records << '\n';
    }
}

void print_metrics(std::ostream& out, const netlog::DeviceStats& totals) {
    const auto error_rate = 100.0L * totals.errors / totals.requests;
    const auto average_us = static_cast<long double>(totals.total_duration_us) / totals.requests;
    out << totals.requests << ' ' << totals.errors << ' ' << error_rate << "% "
        << average_us << ' ' << totals.max_duration_us << '\n';
}

void print_report(std::ostream& out, const netlog::Stats& stats, std::size_t top_k) {
    out << "devices:\n"
        << "device_id requests errors error_rate avg_us max_us\n";
    out << std::fixed << std::setprecision(2);
    for (const auto& [id, device] : stats.devices()) {
        out << id << ' ';
        print_metrics(out, device);
    }

    if (stats.bucket_seconds() > 0) {
        out << "time_windows (" << stats.bucket_seconds() << " seconds each):\n"
            << "start_ts requests errors error_rate avg_us max_us\n";
        for (const auto& [start, totals] : stats.buckets()) {
            out << start << ' ';
            print_metrics(out, totals);
        }
    }

    out << "top_events (up to " << top_k << "):\n";
    for (const auto& [event, count] : stats.top_events(top_k)) {
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

struct FileSegment {
    std::size_t file_id;
    std::uintmax_t begin;
    std::uintmax_t end;
};

constexpr std::uintmax_t segment_bytes = 16 * 1024 * 1024;

std::uintmax_t next_line_start(std::ifstream& input, std::uintmax_t cut,
                               std::uintmax_t file_size) {
    input.seekg(static_cast<std::streamoff>(cut - 1));
    const auto previous = input.get();
    if (!input) {
        throw std::ios_base::failure("Cannot locate segment boundary");
    }
    if (previous == '\n') {
        return cut;
    }
    input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    if (input.bad() || (input.fail() && !input.eof())) {
        throw std::ios_base::failure("Cannot read segment boundary");
    }
    if (input.eof()) {
        return file_size;
    }
    const auto position = static_cast<std::streamoff>(input.tellg());
    if (position < 0 || static_cast<std::uintmax_t>(position) > file_size) {
        throw std::ios_base::failure("Unexpected segment boundary");
    }
    return static_cast<std::uintmax_t>(position);
}

std::vector<FileSegment> plan_segments(const std::vector<fs::path>& files) {
    std::vector<FileSegment> segments;
    for (std::size_t id = 0; id < files.size(); ++id) {
        const auto& path = files[id];
        const auto size = fs::file_size(path);
        if (size <= segment_bytes) {
            segments.push_back({id, 0, size});
            continue;
        }

        if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max())) {
            throw std::ios_base::failure("File is too large to seek: " + path.string());
        }
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) {
            throw std::ios_base::failure("Cannot open input file: " + path.string());
        }

        std::uintmax_t begin = 0;
        while (size - begin > segment_bytes) {
            // 从上一个对齐后的行首切下一段，长行只需越过一次。
            const auto end = next_line_start(input, begin + segment_bytes, size);
            segments.push_back({id, begin, end});
            begin = end;
        }
        if (begin != size) {
            segments.push_back({id, begin, size});
        }
    }
    return segments;
}

struct BadLine {
    std::uint64_t local_line;
    netlog::ParseError error;
};

struct SegmentResult {
    ReadSummary summary;
    std::vector<BadLine> bad_lines;
    FileStatus status = FileStatus::Ok;
    std::exception_ptr exception;
};

void print_bad_line(std::ostream& out, const fs::path& path, std::uint64_t line_number,
                    netlog::ParseError error) {
    out << path.string() << ':' << line_number << ": " << netlog::error_message(error) << '\n';
}

void print_file_error(std::ostream& out, const fs::path& path, FileStatus status) {
    if (status == FileStatus::OpenFailed) {
        out << "Cannot open input file: " << path.string() << '\n';
    } else if (status == FileStatus::ReadFailed) {
        out << "Failed while reading input file: " << path.string() << " (summary is partial)\n";
    }
}

template <typename NextLine, typename ReportBadLine>
void read_records(NextLine next_line, ReportBadLine report_bad_line,
                  ReadSummary& summary, netlog::Stats& stats, const Filter& filter) {
    std::uint64_t line_number = 0;
    std::string line;
    while (next_line(line)) {
        ++line_number;
        ++summary.read_lines;
        const auto result = netlog::parse_line(line);
        if (const auto* error = std::get_if<netlog::ParseError>(&result)) {
            ++summary.invalid_records;
            report_bad_line(line_number, *error);
            continue;
        }
        ++summary.valid_records;
        const auto& record = std::get<netlog::Record>(result);
        if (filter.matches(record)) {
            stats.add(record);
            ++summary.matched_records;
        }
    }
}

FileStatus analyze_file(const fs::path& path, ReadSummary& summary, netlog::Stats& stats,
                        std::ostream& diagnostics, const Filter& filter) {
    // binary 保留原始字节；getline 按 LF 分行，解析器接受行末 CR。
    // ifstream 在离开作用域时自动关闭文件（RAII）。
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        print_file_error(diagnostics, path, FileStatus::OpenFailed);
        return FileStatus::OpenFailed;
    }

    read_records(
        [&](std::string& line) { return static_cast<bool>(std::getline(input, line)); },
        [&](std::uint64_t number, netlog::ParseError error) {
            print_bad_line(diagnostics, path, number, error);
        },
        summary, stats, filter);

    if (input.bad() || (input.fail() && !input.eof())) {
        print_file_error(diagnostics, path, FileStatus::ReadFailed);
        return FileStatus::ReadFailed;
    }
    return FileStatus::Ok;
}

FileStatus analyze_serial(const std::vector<fs::path>& files, ReadSummary& summary,
                          netlog::Stats& stats, const Filter& filter) {
    for (const auto& path : files) {
        const auto status = analyze_file(path, summary, stats, std::cerr, filter);
        if (status != FileStatus::Ok) {
            return status;
        }
    }
    return FileStatus::Ok;
}

FileStatus analyze_segment(const fs::path& path, const FileSegment& segment,
                           SegmentResult& result, netlog::Stats& stats, const Filter& filter) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return FileStatus::OpenFailed;
    }
    if (segment.begin != 0) {
        input.seekg(static_cast<std::streamoff>(segment.begin));
        if (!input) {
            return FileStatus::ReadFailed;
        }
    }

    auto offset = segment.begin;
    read_records(
        [&](std::string& line) {
            if (offset == segment.end || !std::getline(input, line)) {
                return false;
            }
            // getline 丢掉 LF，CRLF 的 CR 仍在 line 里。
            const auto bytes = static_cast<std::uintmax_t>(line.size()) + (input.eof() ? 0 : 1);
            if (bytes > segment.end - offset) {
                return false;
            }
            offset += bytes;
            return true;
        },
        [&](std::uint64_t number, netlog::ParseError error) {
            result.bad_lines.push_back({number, error});
        },
        result.summary, stats, filter);
    return offset == segment.end && !input.bad() ? FileStatus::Ok : FileStatus::ReadFailed;
}

FileStatus analyze_parallel(const std::vector<fs::path>& files,
                            const std::vector<FileSegment>& segments,
                            std::size_t worker_count, ReadSummary& summary, netlog::Stats& stats,
                            const Filter& filter) {
    // 每个线程独占一张统计表，每个片段独占一个结果槽。
    std::vector<netlog::Stats> local_stats(worker_count, netlog::Stats(stats.bucket_seconds()));
    std::vector<SegmentResult> results(segments.size());
    std::atomic<std::size_t> next_task{0};

    const auto work = [&](std::size_t worker_id) {
        auto& local = local_stats[worker_id];
        for (;;) {
            const auto index = next_task.fetch_add(1, std::memory_order_relaxed);
            if (index >= segments.size()) {
                return;
            }
            const auto& segment = segments[index];
            auto& result = results[index];
            try {
                result.status = analyze_segment(files[segment.file_id], segment, result, local, filter);
                if (result.status == FileStatus::Ok) {
                    continue;
                }
            } catch (...) {
                result.exception = std::current_exception();
            }
            // 已领取的片段继续读，停止分发新片段。
            next_task.store(segments.size(), std::memory_order_relaxed);
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
        next_task.store(segments.size(), std::memory_order_relaxed);
        startup_failure = std::current_exception();
    }
    for (auto& worker : workers) {
        worker.join();
    }
    if (startup_failure) {
        std::rethrow_exception(startup_failure);
    }

    // 任务列表按文件和字节偏移排列，实际完成顺序不影响行号。
    std::uint64_t preceding_lines = 0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const auto& segment = segments[i];
        const auto& path = files[segment.file_id];
        const auto& result = results[i];
        if (i == 0 || segment.file_id != segments[i - 1].file_id) {
            preceding_lines = 0;
        }
        for (const auto& bad : result.bad_lines) {
            print_bad_line(std::cerr, path, preceding_lines + bad.local_line, bad.error);
        }
        if (result.exception) {
            std::rethrow_exception(result.exception);
        }
        summary.merge(result.summary);
        preceding_lines += result.summary.read_lines;
        if (result.status != FileStatus::Ok) {
            print_file_error(std::cerr, path, result.status);
            return result.status;
        }
    }
    for (const auto& local : local_stats) {
        stats.merge(local);
    }
    return FileStatus::Ok;
}

int analyze_input(const fs::path& input, const AnalysisOptions& options) {
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
    netlog::Stats stats(options.bucket_seconds);
    FileStatus status;
    if (options.threads == 1) {
        status = analyze_serial(files, summary, stats, options.filter);
    } else {
        std::vector<FileSegment> segments;
        try {
            segments = plan_segments(files);
        } catch (const fs::filesystem_error& error) {
            std::cerr << "Cannot inspect input file or directory: " << error.what() << '\n';
            return 3;
        } catch (const std::ios_base::failure& error) {
            std::cerr << "Cannot split input: " << error.what() << '\n';
            return 3;
        }
        const auto worker_count = std::min(options.threads, segments.size());
        status = worker_count == 1
            ? analyze_serial(files, summary, stats, options.filter)
            : analyze_parallel(files, segments, worker_count, summary, stats, options.filter);
    }
    if (status == FileStatus::OpenFailed) {
        return 3;
    }
    print_summary(summary, options.filter.active());
    if (status == FileStatus::ReadFailed) {
        return 3;
    }
    print_report(std::cout, stats, options.top_k);
    return 0;
}

bool parse_positive_size(std::string_view text, std::size_t& count) {
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, count);
    return result.ec == std::errc{} && result.ptr == end && count > 0;
}

bool parse_seconds(std::string_view text, std::int64_t& seconds) {
    if (text.empty() || text.front() < '0' || text.front() > '9') {
        return false;
    }
    const auto* end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, seconds);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

int run(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_usage(std::cout);
        return 0;
    }
    std::string_view input;
    AnalysisOptions options;
    bool has_threads = false;
    bool has_top = false;
    int i = 1;
    for (; i + 1 < argc; i += 2) {
        const std::string_view option = argv[i];
        const std::string_view value = argv[i + 1];
        std::int64_t seconds = 0;
        if (option == "--input" && input.empty() && !value.empty()) {
            input = value;
        } else if (option == "--threads" && !has_threads && parse_positive_size(value, options.threads)) {
            has_threads = true;
        } else if (option == "--top" && !has_top && parse_positive_size(value, options.top_k)) {
            has_top = true;
        } else if (option == "--device" && options.filter.device.empty() && !value.empty()) {
            options.filter.device = value;
        } else if (option == "--since" && !options.filter.since && parse_seconds(value, seconds)) {
            options.filter.since = seconds;
        } else if (option == "--until" && !options.filter.until && parse_seconds(value, seconds)) {
            options.filter.until = seconds;
        } else if (option == "--bucket" && options.bucket_seconds == 0
                   && parse_seconds(value, seconds) && seconds > 0) {
            options.bucket_seconds = seconds;
        } else {
            break;
        }
    }
    const auto& filter = options.filter;
    const bool reversed_range = filter.since && filter.until && *filter.since >= *filter.until;
    if (i != argc || input.empty() || reversed_range) {
        std::cerr << "Invalid command line.\n";
        print_usage(std::cerr);
        return 2;
    }
    return analyze_input(fs::path(input), options);
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
