#include "data_profiler.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <unordered_set>

static std::size_t fraction_digit_count(std::string_view text, char decimal_separator) noexcept {
    const std::size_t dot = text.find_last_of(decimal_separator);
    if (dot == std::string_view::npos) return 0;
    std::size_t digits = 0;
    for (std::size_t index = dot + 1; index < text.size(); ++index) {
        if (std::isdigit(static_cast<unsigned char>(text[index])) == 0) break;
        ++digits;
    }
    return digits;
}

static double ratio(std::size_t hits, std::size_t total) noexcept {
    return total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
}

merope::dataset_profile_t merope::profile_sample(const std::string& dataset_path, const csv_dialect_t& dialect,
                                                   const sample_t& sample) {
    dataset_profile_t profile;
    profile.dataset_path  = dataset_path;
    profile.dialect       = dialect;
    profile.file_size     = sample.file_size;
    profile.sample_rows   = sample.rows.size();
    profile.rows_scanned  = sample.rows_scanned;
    profile.bytes_scanned = sample.bytes_scanned;
    profile.sample_method = sample.method;
    profile.seed          = sample.seed;
    profile.exact         = sample.exact;

    parse_options_t options;
    options.decimal_separator   = dialect.decimal_separator;
    options.thousands_separator = dialect.thousands_separator;

    const std::size_t column_count = dialect.column_count;
    profile.columns.resize(column_count);

    // Cardinality is tracked on the raw text: two spellings of the same number
    // are two distinct values as far as the source file is concerned.
    std::vector<std::unordered_set<std::string>> distinct(column_count);

    for (std::size_t column = 0; column < column_count; ++column) {
        column_profile_t& stats = profile.columns[column];
        stats.physical_index = column;
        stats.physical_name  = column < dialect.column_names.size()
                                   ? dialect.column_names[column]
                                   : "column_" + std::to_string(column);
        stats.min_length = static_cast<std::size_t>(-1);
    }

    // Pass 1: type votes, shape evidence, cardinality.
    std::vector<std::size_t> currency_hits(column_count, 0);
    std::vector<std::size_t> country_hits(column_count, 0);
    std::vector<std::size_t> email_hits(column_count, 0);
    std::vector<std::size_t> percent_hits(column_count, 0);

    for (const std::vector<std::string>& row : sample.rows) {
        // A short record is a probe artefact or a broken line, not evidence.
        if (row.size() != column_count) continue;

        for (std::size_t column = 0; column < column_count; ++column) {
            column_profile_t&      stats = profile.columns[column];
            const std::string_view field = trim(row[column]);
            ++stats.row_count;

            // Broad tier on purpose: this pass runs before the type is known,
            // and it is the pass the type is inferred from. A numeric column
            // peppered with "-" has to keep inferring as numeric. The narrow
            // tier applies from the second pass on, once inferred_type exists.
            if (looks_like_null(field)) {
                ++stats.null_count;
                continue;
            }

            if (distinct[column].size() < k_cardinality_cap) {
                distinct[column].emplace(field);
            } else {
                stats.unique_exact = false;
            }

            stats.min_length = std::min(stats.min_length, field.size());
            stats.max_length = std::max(stats.max_length, field.size());
            if (stats.examples.size() < k_example_count) {
                const std::string candidate(field);
                if (std::find(stats.examples.begin(), stats.examples.end(), candidate) ==
                    stats.examples.end()) {
                    stats.examples.push_back(candidate);
                }
            }

            std::int64_t   as_int  = 0;
            double         as_real = 0.0;
            bool           as_bool = false;
            date_pattern_t pattern = date_pattern_t::none;

            if (parse_int64(field, options, as_int)) {
                ++stats.int_hits;
                if (as_int < 0) stats.all_non_negative = false;
            }
            if (parse_float64(field, options, as_real)) {
                ++stats.float_hits;
                if (as_real < 0.0) stats.all_non_negative = false;
            }
            if (parse_bool(field, as_bool)) ++stats.bool_hits;
            if (parse_decimal(field, options, as_int)) ++stats.decimal_hits;
            if (parse_date(field, as_int, pattern)) {
                ++stats.date_hits;
                if (stats.date_pattern == date_pattern_t::none) stats.date_pattern = pattern;
            }
            if (parse_datetime(field, as_int, pattern)) {
                ++stats.datetime_hits;
                if (stats.date_pattern == date_pattern_t::none) stats.date_pattern = pattern;
            }

            if (looks_like_currency(field))     ++currency_hits[column];
            if (looks_like_country_code(field)) ++country_hits[column];
            if (looks_like_email(field))        ++email_hits[column];
            if (looks_like_percentage(field))   ++percent_hits[column];
            if (fraction_digit_count(field, options.decimal_separator) == 2) ++stats.two_decimal_hits;
        }
    }

    // Decide the physical type from the votes.
    for (std::size_t column = 0; column < column_count; ++column) {
        column_profile_t& stats = profile.columns[column];
        const std::size_t non_null = stats.row_count - stats.null_count;

        stats.unique_count = distinct[column].size();
        stats.unique_ratio = ratio(stats.unique_count, non_null);
        if (stats.min_length == static_cast<std::size_t>(-1)) stats.min_length = 0;

        stats.currency_ratio   = ratio(currency_hits[column], non_null);
        stats.country_ratio    = ratio(country_hits[column], non_null);
        stats.email_ratio      = ratio(email_hits[column], non_null);
        stats.percentage_ratio = ratio(percent_hits[column], non_null);

        if (non_null == 0) {
            stats.inferred_type = data_type_t::utf8;
            continue;
        }

        auto accepts = [&](std::size_t hits) { return ratio(hits, non_null) >= k_type_accept_ratio; };

        if (accepts(stats.bool_hits) && stats.unique_count <= 2) {
            stats.inferred_type = data_type_t::boolean;
        } else if (accepts(stats.int_hits)) {
            stats.inferred_type = data_type_t::int64;
        } else if (accepts(stats.date_hits)) {
            stats.inferred_type = data_type_t::date;
        } else if (accepts(stats.datetime_hits)) {
            stats.inferred_type = data_type_t::datetime;
        } else if (accepts(stats.decimal_hits) &&
                   (stats.currency_ratio > 0.10 || ratio(stats.two_decimal_hits, non_null) > 0.50)) {
            // Money keeps its exact representation. Guessing FLOAT64 here is
            // how sums quietly stop adding up.
            stats.inferred_type = data_type_t::decimal;
        } else if (accepts(stats.float_hits)) {
            stats.inferred_type = data_type_t::float64;
        } else if (stats.unique_count <= k_categorical_max &&
                   stats.unique_ratio <= k_categorical_ratio) {
            stats.inferred_type = data_type_t::categorical;
        } else {
            stats.inferred_type = data_type_t::utf8;
        }
    }

    // Pass 2: extremes and mean, in the representation the type actually uses.
    std::vector<numeric_accumulator_t> accumulators(column_count);
    std::vector<std::string>           text_min(column_count);
    std::vector<std::string>           text_max(column_count);
    std::vector<bool>                  text_seen(column_count, false);

    for (const std::vector<std::string>& row : sample.rows) {
        if (row.size() != column_count) continue;
        for (std::size_t column = 0; column < column_count; ++column) {
            const column_profile_t& stats = profile.columns[column];
            const std::string_view  field = trim(row[column]);
            if (looks_like_null(field, is_string_like(stats.inferred_type))) continue;

            std::int64_t as_int  = 0;
            double       as_real = 0.0;
            std::string  as_text;
            if (!parse_as(field, stats.inferred_type, options, as_int, as_real, as_text)) continue;

            switch (storage_kind_of(stats.inferred_type)) {
            case storage_kind_t::integer: accumulators[column].offer_int(as_int);  break;
            case storage_kind_t::real:    accumulators[column].offer_real(as_real); break;
            case storage_kind_t::text:
                if (!text_seen[column]) {
                    text_min[column] = text_max[column] = as_text;
                    text_seen[column] = true;
                } else {
                    if (as_text < text_min[column]) text_min[column] = as_text;
                    if (as_text > text_max[column]) text_max[column] = as_text;
                }
                break;
            case storage_kind_t::none: break;
            }
        }
    }

    for (std::size_t column = 0; column < column_count; ++column) {
        column_profile_t&            stats       = profile.columns[column];
        const numeric_accumulator_t& accumulator = accumulators[column];

        switch (storage_kind_of(stats.inferred_type)) {
        case storage_kind_t::integer:
            if (accumulator.seen) {
                stats.has_min = stats.has_max = true;
                stats.min_value = cell_value_t{accumulator.int_min};
                stats.max_value = cell_value_t{accumulator.int_max};
            }
            break;
        case storage_kind_t::real:
            if (accumulator.seen) {
                stats.has_min = stats.has_max = true;
                stats.min_value = cell_value_t{accumulator.real_min};
                stats.max_value = cell_value_t{accumulator.real_max};
            }
            break;
        case storage_kind_t::text:
            if (text_seen[column]) {
                stats.has_min = stats.has_max = true;
                stats.min_value = cell_value_t{text_min[column]};
                stats.max_value = cell_value_t{text_max[column]};
            }
            break;
        case storage_kind_t::none:
            break;
        }

        // A mean is only meaningful for a magnitude. Averaging identifiers or
        // dates produces a number that means nothing, so it is not reported.
        const bool meaningful = stats.inferred_type == data_type_t::int64 ||
                                stats.inferred_type == data_type_t::float64 ||
                                stats.inferred_type == data_type_t::decimal;
        if (meaningful && accumulator.count > 0) {
            stats.has_mean = true;
            stats.mean = static_cast<double>(accumulator.sum / static_cast<long double>(accumulator.count));
            if (stats.inferred_type == data_type_t::decimal) {
                stats.mean /= static_cast<double>(k_money_factor);
            }
        }
    }

    // Row count estimate for the whole file, from the bytes we did read.
    if (profile.exact) {
        profile.estimated_total_rows = profile.rows_scanned;
    } else if (profile.bytes_scanned > 0 && profile.rows_scanned > 0) {
        const double bytes_per_row = static_cast<double>(profile.bytes_scanned) /
                                     static_cast<double>(profile.rows_scanned);
        profile.estimated_total_rows =
            static_cast<std::uint64_t>(static_cast<double>(profile.file_size) / bytes_per_row);
    }

    return profile;
}

std::string merope::format_profile(const dataset_profile_t& profile) {
    std::ostringstream out;
    char buffer[128];

    out << "DATASET PROFILE\n";
    out << "  path:            " << profile.dataset_path << "\n";
    out << "  file size:       " << profile.file_size << " bytes\n";
    out << "  sample method:   " << to_string(profile.sample_method) << " (seed " << profile.seed << ")\n";
    out << "  sample rows:     " << profile.sample_rows << "\n";
    out << "  rows scanned:    " << profile.rows_scanned << "\n";
    if (profile.exact) {
        out << "  total rows:      " << profile.estimated_total_rows << " (exact, whole file scanned)\n";
    } else {
        out << "  total rows:      ~" << profile.estimated_total_rows << " (estimate from sample)\n";
    }
    out << "\n";

    for (const column_profile_t& stats : profile.columns) {
        out << "  " << stats.physical_name << "\n";
        out << "    type:          " << to_string(stats.inferred_type) << "\n";
        out << "    row_count:     " << stats.row_count << "\n";
        out << "    null_count:    " << stats.null_count << "\n";
        out << "    unique_count:  " << stats.unique_count
            << (stats.unique_exact ? "" : "+ (cardinality cap reached)") << "\n";
        std::snprintf(buffer, sizeof(buffer), "%.4f", stats.unique_ratio);
        out << "    unique_ratio:  " << buffer
            << (profile.exact ? "" : "   (sample estimate)") << "\n";
        if (stats.has_min) {
            out << "    min:           " << cell_to_display(stats.min_value, stats.inferred_type)
                << (profile.exact ? "" : "   (sample estimate)") << "\n";
        }
        if (stats.has_max) {
            out << "    max:           " << cell_to_display(stats.max_value, stats.inferred_type)
                << (profile.exact ? "" : "   (sample estimate)") << "\n";
        }
        if (stats.has_mean) {
            std::snprintf(buffer, sizeof(buffer), "%.4f", stats.mean);
            out << "    mean:          " << buffer
                << (profile.exact ? "" : "   (sample estimate)") << "\n";
        }
        if (stats.date_pattern != date_pattern_t::none) {
            out << "    date pattern:  " << to_string(stats.date_pattern) << "\n";
        }
        if (!stats.examples.empty()) {
            out << "    examples:      ";
            for (std::size_t index = 0; index < stats.examples.size(); ++index) {
                if (index > 0) out << ", ";
                out << stats.examples[index];
            }
            out << "\n";
        }
        out << "\n";
    }
    return out.str();
}


void merope::numeric_accumulator_t::offer_int(std::int64_t value) {
    if (!seen) { int_min = int_max = value; seen = true; }
    int_min = std::min(int_min, value);
    int_max = std::max(int_max, value);
    sum += static_cast<long double>(value);
    ++count;
}

void merope::numeric_accumulator_t::offer_real(double value) {
    if (!seen) { real_min = real_max = value; seen = true; }
    real_min = std::min(real_min, value);
    real_max = std::max(real_max, value);
    sum += static_cast<long double>(value);
    ++count;
}
