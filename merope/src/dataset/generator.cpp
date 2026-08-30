#include "generator.h"

#include "../core/types.h"

#include <cstdio>
#include <fstream>
#include <random>
#include <stdexcept>

static constexpr const char* k_countries[] = {"SK", "CZ", "DE", "AT", "PL", "HU"};
static constexpr std::size_t k_country_count = sizeof(k_countries) / sizeof(k_countries[0]);

merope::generator_stats_t merope::generate_dataset(const std::string& path, const generator_options_t& options) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("cannot write dataset: " + path);

    generator_stats_t stats;
    std::mt19937_64   rng(options.seed);
    bool seen_amount = false;
    std::vector<bool> country_seen(k_country_count, false);

    std::uniform_int_distribution<int>          user(1, 50000);
    std::uniform_int_distribution<int>          cents(1, 2000000);   // up to 20000.00
    std::uniform_int_distribution<std::size_t>  country(0, k_country_count - 1);
    std::uniform_int_distribution<std::int64_t> day(days_from_civil(2024, 1, 1),
                                                    days_from_civil(2026, 12, 31));
    std::uniform_int_distribution<int>          second(0, 86399);
    std::uniform_real_distribution<double>      chance(0.0, 1.0);

    std::string buffer;
    buffer.reserve(1 << 20);
    char scratch[128];

    if (options.write_header) {
        buffer += "transaction_id";
        buffer += options.delimiter;
        buffer += "user_id";
        buffer += options.delimiter;
        buffer += "amount";
        buffer += options.delimiter;
        buffer += "country";
        buffer += options.delimiter;
        buffer += "timestamp\n";
    }

    for (std::uint64_t row = 0; row < options.rows; ++row) {
        const bool corrupt = options.corrupt_fraction > 0.0 && chance(rng) < options.corrupt_fraction;
        const bool null_country = options.null_fraction > 0.0 && chance(rng) < options.null_fraction;

        const std::int64_t amount_cents = cents(rng);
        const std::int64_t days         = day(rng);
        int year = 0; unsigned month = 0, day_of_month = 0;
        civil_from_days(days, year, month, day_of_month);
        const int seconds = second(rng);

        std::snprintf(scratch, sizeof(scratch), "%llu", static_cast<unsigned long long>(row + 1));
        buffer += scratch;
        buffer += options.delimiter;

        std::snprintf(scratch, sizeof(scratch), "%d", user(rng));
        buffer += scratch;
        buffer += options.delimiter;

        std::snprintf(scratch, sizeof(scratch), "%lld%c%02lld",
                      static_cast<long long>(amount_cents / 100), options.decimal_separator,
                      static_cast<long long>(amount_cents % 100));
        buffer += scratch;

        if (corrupt) {
            buffer += '\n';
            ++stats.corrupted_rows;
        } else {
            buffer += options.delimiter;
            if (null_country) {
                ++stats.null_countries;
            } else {
                const std::size_t which = country(rng);
                country_seen[which] = true;
                buffer += k_countries[which];
            }
            buffer += options.delimiter;

            std::snprintf(scratch, sizeof(scratch), "%04d-%02u-%02uT%02d:%02d:%02d", year, month,
                          day_of_month, seconds / 3600, (seconds / 60) % 60, seconds % 60);
            buffer += scratch;
            buffer += '\n';

            const std::int64_t scaled = amount_cents * (k_money_factor / 100);
            if (!seen_amount) {
                stats.min_amount_scaled = stats.max_amount_scaled = scaled;
                seen_amount = true;
            }
            if (scaled < stats.min_amount_scaled) stats.min_amount_scaled = scaled;
            if (scaled > stats.max_amount_scaled) stats.max_amount_scaled = scaled;
            stats.total_amount_scaled += scaled;
            if (year == 2025) ++stats.rows_in_2025;
        }
        ++stats.rows_written;

        if (buffer.size() >= (1u << 20)) {
            stream.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            stats.bytes_written += buffer.size();
            buffer.clear();
        }
    }

    if (!buffer.empty()) {
        stream.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        stats.bytes_written += buffer.size();
    }
    for (const bool seen : country_seen) {
        if (seen) ++stats.distinct_countries;
    }
    if (!stream) throw std::runtime_error("failed while writing " + path);
    return stats;
}


static constexpr merope::generated_column_t k_generated_schema[] = {
    {"transaction_id", "INT64",       "IDENTIFIER"},
    {"user_id",        "INT64",       "IDENTIFIER"},
    {"amount",         "DECIMAL",     "MONETARY"},
    {"country",        "CATEGORICAL", "COUNTRY"},
    {"timestamp",      "DATETIME",    "TIMESTAMP"},
};

const merope::generated_column_t* merope::generated_schema(std::size_t& count) noexcept {
    count = sizeof(k_generated_schema) / sizeof(k_generated_schema[0]);
    return k_generated_schema;
}
