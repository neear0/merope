// merope - AI-assisted big data processing engine
// core/types.h - physical and semantic type system (spec chapter 4.7)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

namespace merope {

// Fixed-point scale used for DECIMAL / MONEY. Monetary values are stored as
// int64 scaled by 10^k_money_scale so that SUM stays exact.
inline constexpr int          k_money_scale  = 4;
inline constexpr std::int64_t k_money_factor = 10000;

// Physical storage type of a column. This is what the engine actually operates
// on; it is decided by the profiler from sampled data, never hardcoded.
enum class data_type_t : std::uint8_t {
    unknown = 0,
    int64,        // counts, identifiers
    float64,      // continuous quantities (not the default for money)
    boolean,      // true/false, also 0/1, yes/no
    utf8,         // free text
    categorical,  // low cardinality strings (country, status)
    date,         // days since 1970-01-01
    datetime,     // seconds since 1970-01-01T00:00:00Z
    decimal       // monetary amounts, int64 scaled by k_money_factor
};

// Meaning of a column, proposed by heuristics and/or AI and confirmed by the
// user. `unknown` is always a legal answer (spec 4.4 / 4.5).
enum class semantic_type_t : std::uint8_t {
    unknown = 0,
    identifier,
    quantity,
    monetary,
    percentage,
    country,
    category,
    status,
    date_value,
    timestamp,
    text,
    flag,
    email
};

// How a value of a given physical type is physically stored in a column block.
enum class storage_kind_t : std::uint8_t { none, integer, real, text };

storage_kind_t storage_kind_of(data_type_t type) noexcept;
bool           is_numeric(data_type_t type) noexcept;
bool           is_temporal(data_type_t type) noexcept;
bool           is_string_like(data_type_t type) noexcept;

const char* to_string(data_type_t type) noexcept;
const char* to_string(semantic_type_t type) noexcept;

data_type_t     data_type_from_string(const std::string& name) noexcept;
semantic_type_t semantic_type_from_string(const std::string& name) noexcept;

// A single scalar value, used for literals, group keys and result cells.
// Engine-internal columns are stored columnar; this type is for the edges.
using cell_value_t = std::variant<std::monostate, std::int64_t, double, bool, std::string>;

bool        is_null(const cell_value_t& value) noexcept;
std::string cell_to_display(const cell_value_t& value, data_type_t type);

// Calendar helpers. Deliberately time-zone free so that reports reproduce.
void         civil_from_days(std::int64_t days, int& year, unsigned& month, unsigned& day) noexcept;
std::int64_t days_from_civil(int year, unsigned month, unsigned day) noexcept;

} // namespace merope
