#include "sampler.h"

#include "csv_reader.h"

#include <algorithm>
#include <filesystem>
#include <random>

// Reservoir sampling, algorithm R. Every row seen has the same probability of
// ending up in the reservoir, whatever the file ordering.
static void offer_to_reservoir(std::vector<std::vector<std::string>>& reservoir, std::size_t capacity,
                        std::uint64_t seen, std::vector<std::string> row, std::mt19937_64& rng) {
    if (reservoir.size() < capacity) {
        reservoir.push_back(std::move(row));
        return;
    }
    std::uniform_int_distribution<std::uint64_t> pick(0, seen - 1);
    const std::uint64_t slot = pick(rng);
    if (slot < capacity) reservoir[static_cast<std::size_t>(slot)] = std::move(row);
}

const char* merope::to_string(sample_method_t method) noexcept {
    switch (method) {
    case sample_method_t::head_and_offsets: return "head + random line-aligned offsets";
    case sample_method_t::full_scan_reservoir:
    default:                                return "reservoir over a full scan";
    }
}

merope::sample_t merope::sample_dataset(const std::string& path, const csv_dialect_t& dialect,
                                          const sample_options_t& options) {
    sample_t result;
    result.seed = options.seed;

    std::error_code ec;
    result.file_size = std::filesystem::file_size(path, ec);
    if (ec) result.file_size = 0;

    std::mt19937_64 rng(options.seed);

    // The head block always comes along: it is the only evidence about the
    // physical start of the file, and cheap.
    std::vector<std::string> fields;
    {
        c_record_reader reader(path, dialect, 0, k_whole_file, true);
        while (result.rows.size() < options.head_rows && reader.next(fields)) {
            result.rows.push_back(fields);
        }
        result.rows_scanned  += reader.stats().rows_read;
        result.bytes_scanned += reader.stats().bytes_consumed;
    }

    const bool small_enough = result.file_size <= options.full_scan_limit_bytes;
    if (small_enough) {
        result.method = sample_method_t::full_scan_reservoir;

        std::vector<std::vector<std::string>> reservoir;
        reservoir.reserve(std::min<std::size_t>(options.target_rows, 1 << 16));

        c_record_reader reader(path, dialect, 0, k_whole_file, true);
        std::uint64_t seen = 0;
        while (reader.next(fields)) {
            ++seen;
            offer_to_reservoir(reservoir, options.target_rows, seen, fields, rng);
        }
        result.rows_scanned  = seen;
        result.bytes_scanned = reader.stats().bytes_consumed;
        result.exact         = seen <= options.target_rows;
        result.rows          = std::move(reservoir);
        return result;
    }

    // Large file: probe random offsets. Each probe snaps forward to the next
    // newline, so no probe ever starts in the middle of a record.
    result.method = sample_method_t::head_and_offsets;
    result.exact  = false;

    const std::uint64_t lowest = std::min<std::uint64_t>(result.bytes_scanned, result.file_size);
    if (result.file_size > lowest) {
        std::uniform_int_distribution<std::uint64_t> offset(lowest, result.file_size - 1);
        std::vector<std::uint64_t> probes;
        probes.reserve(options.probe_count);
        for (std::size_t k = 0; k < options.probe_count; ++k) probes.push_back(offset(rng));
        std::sort(probes.begin(), probes.end());

        for (const std::uint64_t begin : probes) {
            if (result.rows.size() >= options.target_rows) break;
            c_record_reader reader(path, dialect, begin, k_whole_file, false);
            std::size_t taken = 0;
            while (taken < options.probe_rows && result.rows.size() < options.target_rows &&
                   reader.next(fields)) {
                // A probe can land on the very last, unterminated line and pick
                // up a short record; the profiler rejects those by width.
                result.rows.push_back(fields);
                ++taken;
            }
            result.rows_scanned  += reader.stats().rows_read;
            result.bytes_scanned += reader.stats().bytes_consumed;
        }
    }

    return result;
}

