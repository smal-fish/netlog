#include "netlog/parser.h"
#include "netlog/stats.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct ReadSummary {
    std::uint64_t read_lines = 0;
    std::uint64_t valid_records = 0;
    std::uint64_t invalid_records = 0;
};

void print_usage(std::ostream& out) {
    out << "Usage: netlog --input <file-or-directory>\n"
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

int analyze_file(const fs::path& path, ReadSummary& summary, netlog::Stats& stats) {
    // binary 保留原始字节；getline 按 LF 分行，解析器接受行末 CR。
    // ifstream 在离开作用域时自动关闭文件（RAII）。
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        std::cerr << "Cannot open input file: " << path.string() << '\n';
        return 3;
    }

    std::uint64_t line_number = 0;
    std::string line;
    while (std::getline(input, line)) {
        ++line_number;
        ++summary.read_lines;
        const auto result = netlog::parse_line(line);
        if (const auto* error = std::get_if<netlog::ParseError>(&result)) {
            ++summary.invalid_records;
            std::cerr << path.string() << ':' << line_number << ": "
                      << netlog::error_message(*error) << '\n';
            continue;
        }
        stats.add(std::get<netlog::Record>(result));
        ++summary.valid_records;
    }

    if (input.bad() || (input.fail() && !input.eof())) {
        std::cerr << "Failed while reading input file: " << path.string()
                  << " (summary is partial)\n";
        print_summary(summary);
        return 3;
    }
    return 0;
}

int analyze_input(const fs::path& input) {
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
    for (const auto& path : files) {
        const int status = analyze_file(path, summary, stats);
        if (status != 0) {
            return status;
        }
    }
    print_summary(summary);
    print_report(std::cout, stats);
    return 0;
}

int run(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_usage(std::cout);
        return 0;
    }
    if (argc != 3 || std::string_view(argv[1]) != "--input"
        || std::string_view(argv[2]).empty()) {
        std::cerr << "Invalid command line.\n";
        print_usage(std::cerr);
        return 2;
    }
    return analyze_input(argv[2]);
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
