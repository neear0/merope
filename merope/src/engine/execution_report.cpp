#include "execution_report.h"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

std::uint64_t merope::peak_memory_bytes() noexcept {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
        return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
    }
    return 0;
#else
    return 0;
#endif
}

double merope::execution_report_t::throughput_mb_per_second() const noexcept {
    if (processing_seconds <= 0.0) return 0.0;
    return static_cast<double>(bytes_scanned) / processing_seconds / (1024.0 * 1024.0);
}

double merope::execution_report_t::speedup() const noexcept {
    if (!has_baseline || processing_seconds <= 0.0) return 0.0;
    return baseline_seconds / processing_seconds;
}

double merope::execution_report_t::efficiency() const noexcept {
    const double gain = speedup();
    if (gain <= 0.0 || workers == 0) return 0.0;
    return gain / static_cast<double>(workers);
}

std::string merope::format_bytes(std::uint64_t bytes) {
    static const char* const k_units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double      value = static_cast<double>(bytes);
    std::size_t unit  = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(k_units) / sizeof(k_units[0])) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.2f %s", value, k_units[unit]);
    return buffer;
}

std::string merope::format_count(std::uint64_t value) {
    const std::string digits = std::to_string(value);
    std::string       out;
    out.reserve(digits.size() + digits.size() / 3);
    // Built from the right, so no unsigned arithmetic can wrap around.
    std::size_t placed = 0;
    for (std::size_t index = digits.size(); index > 0; --index) {
        if (placed > 0 && placed % 3 == 0) out.push_back(',');
        out.push_back(digits[index - 1]);
        ++placed;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::string merope::format_report(const execution_report_t& report) {
    std::ostringstream out;
    char buffer[64];

    auto line = [&](const char* label, const std::string& value) {
        std::string padded = label;
        if (padded.size() < 18) padded.append(18 - padded.size(), ' ');
        out << padded << value << "\n";
    };

    out << "PROCESSING REPORT\n";
    line("Dataset size:", format_bytes(report.dataset_size_bytes));
    line("Records:", format_count(report.records_processed));
    if (report.rows_after_filter != report.records_processed) {
        line("After filter:", format_count(report.rows_after_filter));
    }
    if (report.bad_rows > 0) {
        line("Bad rows:", format_count(report.bad_rows) + "  (policy: " + report.bad_row_policy + ")");
    }
    if (report.quarantined_rows > 0) {
        line("Quarantined:", format_count(report.quarantined_rows));
    }
    line("Workers:", std::to_string(report.workers));
    line("Partitions:", std::to_string(report.partitions) +
                            (report.partition_note.empty() ? "" : "  (" + report.partition_note + ")"));
    if (report.groups > 0) line("Groups:", format_count(report.groups));

    std::snprintf(buffer, sizeof(buffer), "%.2f s", report.processing_seconds);
    line("Processing time:", buffer);

    std::snprintf(buffer, sizeof(buffer), "%.1f MB/s", report.throughput_mb_per_second());
    line("Throughput:", buffer);

    if (report.peak_memory > 0) line("Peak memory:", format_bytes(report.peak_memory));

    if (report.has_baseline) {
        std::snprintf(buffer, sizeof(buffer), "%.2f s", report.baseline_seconds);
        line("Single-thread:", buffer);
        std::snprintf(buffer, sizeof(buffer), "%.2f s", report.processing_seconds);
        line("Parallel:", buffer);
        std::snprintf(buffer, sizeof(buffer), "%.2fx", report.speedup());
        line("Speedup:", buffer);
        std::snprintf(buffer, sizeof(buffer), "%.2f", report.efficiency());
        line("Efficiency:", buffer);
    }

    return out.str();
}


std::uint64_t merope::current_memory_bytes() noexcept {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
        return static_cast<std::uint64_t>(counters.WorkingSetSize);
    }
    return 0;
#else
    return 0;
#endif
}

merope::c_memory_sampler::c_memory_sampler(unsigned interval_ms) : m_interval_ms(interval_ms) {}

merope::c_memory_sampler::~c_memory_sampler() { stop(); }

void merope::c_memory_sampler::start() {
    if (m_running.exchange(true)) return;
    m_peak.store(current_memory_bytes(), std::memory_order_relaxed);
    m_thread = std::thread([this] { loop(); });
}

void merope::c_memory_sampler::stop() {
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
    // One last reading, in case the peak landed between samples.
    const std::uint64_t last = current_memory_bytes();
    if (last > m_peak.load(std::memory_order_relaxed)) {
        m_peak.store(last, std::memory_order_relaxed);
    }
}

void merope::c_memory_sampler::loop() {
    while (m_running.load(std::memory_order_relaxed)) {
        const std::uint64_t now = current_memory_bytes();
        if (now > m_peak.load(std::memory_order_relaxed)) {
            m_peak.store(now, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(m_interval_ms));
    }
}
