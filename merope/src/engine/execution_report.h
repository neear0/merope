// engine/execution_report.h - what the engine reports back about a run
// (spec 6.4). Every number here is measured, never estimated.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace merope {

// Peak working set of this process, in bytes. Returns 0 when the platform
// does not expose it. This counter only ever rises, so it cannot separate one
// run from the next inside a single process.
std::uint64_t peak_memory_bytes() noexcept;

// Working set right now, in bytes.
std::uint64_t current_memory_bytes() noexcept;

// Samples the working set on its own thread so a single run has a peak of its
// own. The benchmark suite needs this to show that peak memory stays flat as
// the dataset grows, which the process wide counter cannot demonstrate.
class c_memory_sampler {
public:
    explicit c_memory_sampler(unsigned interval_ms = 5);
    ~c_memory_sampler();

    c_memory_sampler(const c_memory_sampler&)            = delete;
    c_memory_sampler& operator=(const c_memory_sampler&) = delete;

    void start();
    void stop();

    std::uint64_t peak() const noexcept { return m_peak.load(std::memory_order_relaxed); }

private:
    void loop();

    unsigned                   m_interval_ms;
    std::thread                m_thread;
    std::atomic<std::uint64_t> m_peak{0};
    std::atomic<bool>          m_running{false};
};

struct execution_report_t {
    std::uint64_t dataset_size_bytes = 0;
    std::uint64_t records_processed  = 0;
    std::uint64_t bad_rows           = 0;
    std::uint64_t quarantined_rows   = 0;
    std::size_t   workers            = 0;
    std::size_t   partitions         = 0;

    double        processing_seconds = 0.0;
    std::uint64_t bytes_scanned      = 0;
    std::uint64_t peak_memory        = 0;
    std::size_t   groups             = 0;
    std::uint64_t rows_after_filter  = 0;

    // Only filled in when a single threaded baseline was actually run.
    bool   has_baseline      = false;
    double baseline_seconds  = 0.0;

    std::string partition_note;
    std::string bad_row_policy;

    double throughput_mb_per_second() const noexcept;
    double speedup() const noexcept;
    double efficiency() const noexcept;
};

std::string format_report(const execution_report_t& report);

// Human readable byte count, e.g. "24.7 GB".
std::string format_bytes(std::uint64_t bytes);
// Thousands-separated integer, e.g. "284,721,932".
std::string format_count(std::uint64_t value);

} // namespace merope
