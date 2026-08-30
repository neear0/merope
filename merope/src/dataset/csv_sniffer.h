#pragma once

#include "csv_format.h"

#include <string>
#include <vector>

namespace merope {

struct sniff_result_t {
    csv_dialect_t            dialect;
    std::uint64_t            file_size_bytes = 0;
    std::size_t              sample_bytes    = 0;
    std::vector<std::string> notes;  // what the sniffer decided, and why
};

inline constexpr std::size_t k_default_sniff_bytes = 256 * 1024;

// Internal to csv_sniffer.cpp: how well one candidate delimiter explains the
// sampled lines. Declared here because declarations belong in headers.
struct delimiter_score_t {
    char        delimiter   = ',';
    std::size_t modal_count = 0;
    double      consistency = 0.0;
    double      score       = 0.0;
};

sniff_result_t sniff_csv(const std::string& path, std::size_t sample_bytes = k_default_sniff_bytes);

}
