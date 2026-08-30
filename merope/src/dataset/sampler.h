#pragma once

#include "csv_format.h"

#include <cstdint>
#include <string>
#include <vector>

namespace merope {

enum class sample_method_t : std::uint8_t {
    full_scan_reservoir,  // small file: every row had an equal chance
    head_and_offsets      // large file: head block plus random line-aligned probes
};

const char* to_string(sample_method_t method) noexcept;

struct sample_options_t {
    std::size_t   target_rows = 10000;
    std::uint64_t seed        = 20260101;  // fixed so experiments reproduce
    std::size_t   head_rows   = 200;       // always taken, for format evidence
    std::size_t   probe_count = 64;        // random offsets, large files only
    std::size_t   probe_rows  = 256;       // rows read at each probe

    // Above this size a full scan just to sample is not worth it.
    std::uint64_t full_scan_limit_bytes = 64ull * 1024 * 1024;
};

struct sample_t {
    std::vector<std::vector<std::string>> rows;
    sample_method_t method        = sample_method_t::full_scan_reservoir;
    std::uint64_t   rows_scanned  = 0;   // rows the sampler actually looked at
    std::uint64_t   file_size     = 0;
    std::uint64_t   bytes_scanned = 0;
    std::uint64_t   seed          = 0;

    // True only when the sampler saw every row, which is the only case where
    // counts and extremes are exact rather than estimates.
    bool exact = false;
};

sample_t sample_dataset(const std::string& path, const csv_dialect_t& dialect,
                        const sample_options_t& options = {});

}
