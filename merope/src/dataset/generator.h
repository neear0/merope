// dataset/generator.h - synthetic datasets for the experiments in spec 12.
// Deterministic for a given seed, so a measurement can be repeated exactly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace merope {

struct generator_options_t {
    std::uint64_t rows        = 100000;
    std::uint64_t seed        = 20260101;
    bool          write_header = true;

    // Fraction of rows that get a deliberately broken field, to exercise the
    // bad row policy.
    double corrupt_fraction = 0.0;
    // Fraction of fields left empty.
    double null_fraction = 0.01;

    char delimiter         = ',';
    char decimal_separator = '.';
};

struct generator_stats_t {
    std::uint64_t rows_written    = 0;
    std::uint64_t bytes_written   = 0;
    std::uint64_t corrupted_rows  = 0;
    // The exact answers, so a test can check the engine rather than trust it,
    // and so the benchmark suite can score inference against known truth.
    std::int64_t  total_amount_scaled = 0;   // sum of amount, scaled by k_money_factor
    std::uint64_t rows_in_2025        = 0;
    std::int64_t  min_amount_scaled   = 0;
    std::int64_t  max_amount_scaled   = 0;
    std::uint64_t distinct_countries  = 0;
    std::uint64_t null_countries      = 0;
};

// What the generator actually wrote, per column, for scoring schema inference.
struct generated_column_t {
    const char* physical_name;
    const char* physical_type;   // as data_type_t spells it
    const char* semantic_type;   // as semantic_type_t spells it
};

// The five columns of a generated dataset, in physical order.
const generated_column_t* generated_schema(std::size_t& count) noexcept;

// Writes transaction_id, user_id, amount, country, timestamp.
generator_stats_t generate_dataset(const std::string& path, const generator_options_t& options);

}
