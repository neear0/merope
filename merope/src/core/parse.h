#pragma once

#include "types.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace merope {

// Layout facts about the file that change how a field is read. Discovered by
// the sniffer, then carried along for every parse.
struct parse_options_t {
    char decimal_separator   = '.';
    char thousands_separator = '\0';  // '\0' means none
};

enum class date_pattern_t : std::uint8_t {
    none = 0,
    iso,          // 2026-01-31
    iso_slash,    // 2026/01/31
    dmy_dot,      // 31.01.2026
    dmy_slash,    // 31/01/2026
    mdy_slash     // 01/31/2026, only when day > 12 rules out dmy
};

const char* to_string(date_pattern_t pattern) noexcept;

std::string_view trim(std::string_view text) noexcept;

bool looks_like_null(std::string_view text, bool text_column = false) noexcept;

bool parse_int64(std::string_view text, const parse_options_t& options, std::int64_t& out) noexcept;
bool parse_float64(std::string_view text, const parse_options_t& options, double& out) noexcept;
bool parse_bool(std::string_view text, bool& out) noexcept;

// Parses into int64 scaled by k_money_factor. Rejects input with more fraction
// digits than the scale rather than silently rounding it away.
bool parse_decimal(std::string_view text, const parse_options_t& options, std::int64_t& out) noexcept;

// Days since the epoch. `pattern` reports which layout matched.
bool parse_date(std::string_view text, std::int64_t& out_days, date_pattern_t& pattern) noexcept;

// Seconds since the epoch. Accepts a bare date too (midnight UTC).
bool parse_datetime(std::string_view text, std::int64_t& out_seconds, date_pattern_t& pattern) noexcept;

// Shape probes used by the heuristics in spec 4.4. They answer "does this look
// like", never "this is".
bool looks_like_currency(std::string_view text) noexcept;
bool looks_like_country_code(std::string_view text) noexcept;
bool looks_like_email(std::string_view text) noexcept;
bool looks_like_percentage(std::string_view text) noexcept;

// Reads a field as `type`, producing the physical representation the engine
// stores. Text types are handed back through `out_text`.
bool parse_as(std::string_view text, data_type_t type, const parse_options_t& options,
              std::int64_t& out_int, double& out_real, std::string& out_text) noexcept;

}
