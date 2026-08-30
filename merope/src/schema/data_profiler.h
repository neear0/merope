// schema/data_profiler.h - per column statistics computed from the sample
// (spec 4.3). Everything here is evidence; nothing here decides meaning.
#pragma once

#include "../core/parse.h"
#include "../core/types.h"
#include "../dataset/csv_format.h"
#include "../dataset/sampler.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace merope {

// Fraction of non-null values that must parse as a type for it to win.
inline constexpr double      k_type_accept_ratio  = 0.99;
inline constexpr std::size_t k_cardinality_cap    = 100000;
inline constexpr std::size_t k_categorical_max    = 256;
inline constexpr double      k_categorical_ratio  = 0.10;
inline constexpr std::size_t k_example_count      = 5;

struct column_profile_t {
    std::string physical_name;
    std::size_t physical_index = 0;

    data_type_t inferred_type = data_type_t::unknown;

    std::size_t row_count    = 0;   // sample rows that had this column
    std::size_t null_count   = 0;
    std::size_t unique_count = 0;
    double      unique_ratio = 0.0;
    // False when the cardinality cap was hit, so unique_count is a lower bound.
    bool        unique_exact = true;

    bool         has_min = false;
    bool         has_max = false;
    cell_value_t min_value;
    cell_value_t max_value;

    bool   has_mean = false;
    double mean     = 0.0;

    std::vector<std::string> examples;

    // How many non-null values each candidate type accepted. Kept so the report
    // can show why a type won and how close the runner up was.
    std::size_t int_hits      = 0;
    std::size_t float_hits    = 0;
    std::size_t bool_hits     = 0;
    std::size_t date_hits     = 0;
    std::size_t datetime_hits = 0;
    std::size_t decimal_hits  = 0;

    date_pattern_t date_pattern = date_pattern_t::none;

    // Shape evidence for the heuristics in spec 4.4.
    double      currency_ratio   = 0.0;
    double      country_ratio    = 0.0;
    double      email_ratio      = 0.0;
    double      percentage_ratio = 0.0;
    std::size_t two_decimal_hits = 0;
    std::size_t min_length       = 0;
    std::size_t max_length       = 0;
    bool        all_non_negative = true;
};

// Internal to data_profiler.cpp. It lives here because declarations belong in
// headers; nothing outside the profiler should reach for it.
struct numeric_accumulator_t {
    bool         seen     = false;
    std::int64_t int_min  = 0;
    std::int64_t int_max  = 0;
    double       real_min = 0.0;
    double       real_max = 0.0;
    long double  sum      = 0.0L;
    std::size_t  count    = 0;

    void offer_int(std::int64_t value);
    void offer_real(double value);
};

struct dataset_profile_t {
    std::string     dataset_path;
    csv_dialect_t   dialect;
    std::uint64_t   file_size      = 0;
    std::size_t     sample_rows    = 0;
    std::uint64_t   rows_scanned   = 0;
    std::uint64_t   bytes_scanned  = 0;
    sample_method_t sample_method  = sample_method_t::full_scan_reservoir;
    std::uint64_t   seed           = 0;

    // True only when the sampler saw the whole file. When false, every count
    // and extreme below is an estimate from the sample and is labelled as one.
    bool          exact                = false;
    std::uint64_t estimated_total_rows = 0;

    std::vector<column_profile_t> columns;
};

dataset_profile_t profile_sample(const std::string& dataset_path, const csv_dialect_t& dialect,
                                 const sample_t& sample);

std::string format_profile(const dataset_profile_t& profile);

}
