#include "types.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

static std::string lower_copy(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Era based civil/day conversion. Deliberately not localtime/gmtime: the engine
// must produce the same numbers on every machine and in every time zone.
void merope::civil_from_days(std::int64_t days, int& year, unsigned& month, unsigned& day) noexcept {
    days += 719468;
    const std::int64_t  era = (days >= 0 ? days : days - 146096) / 146097;
    const std::uint64_t doe = static_cast<std::uint64_t>(days - era * 146097);
    const std::uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t  y   = static_cast<std::int64_t>(yoe) + era * 400;
    const std::uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::uint64_t mp  = (5 * doy + 2) / 153;
    day   = static_cast<unsigned>(doy - (153 * mp + 2) / 5 + 1);
    month = static_cast<unsigned>(mp < 10 ? mp + 3 : mp - 9);
    year  = static_cast<int>(y + (month <= 2 ? 1 : 0));
}

std::int64_t merope::days_from_civil(int year, unsigned month, unsigned day) noexcept {
    std::int64_t y = year;
    y -= (month <= 2) ? 1 : 0;
    const std::int64_t  era = (y >= 0 ? y : y - 399) / 400;
    const std::uint64_t yoe = static_cast<std::uint64_t>(y - era * 400);
    const std::uint64_t mshift = month > 2 ? month - 3u : month + 9u;
    const std::uint64_t doy = (153u * mshift + 2u) / 5u + day - 1u;
    const std::uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

merope::storage_kind_t merope::storage_kind_of(data_type_t type) noexcept {
    switch (type) {
    case data_type_t::int64:
    case data_type_t::boolean:
    case data_type_t::date:
    case data_type_t::datetime:
    case data_type_t::decimal:
        return storage_kind_t::integer;
    case data_type_t::float64:
        return storage_kind_t::real;
    case data_type_t::utf8:
    case data_type_t::categorical:
        return storage_kind_t::text;
    case data_type_t::unknown:
    default:
        return storage_kind_t::none;
    }
}

bool merope::is_numeric(data_type_t type) noexcept {
    return type == data_type_t::int64 || type == data_type_t::float64 || type == data_type_t::decimal;
}

bool merope::is_temporal(data_type_t type) noexcept {
    return type == data_type_t::date || type == data_type_t::datetime;
}

bool merope::is_string_like(data_type_t type) noexcept {
    return type == data_type_t::utf8 || type == data_type_t::categorical;
}

const char* merope::to_string(data_type_t type) noexcept {
    switch (type) {
    case data_type_t::int64:       return "INT64";
    case data_type_t::float64:     return "FLOAT64";
    case data_type_t::boolean:     return "BOOL";
    case data_type_t::utf8:        return "UTF8";
    case data_type_t::categorical: return "CATEGORICAL";
    case data_type_t::date:        return "DATE";
    case data_type_t::datetime:    return "DATETIME";
    case data_type_t::decimal:     return "DECIMAL";
    case data_type_t::unknown:
    default:                       return "UNKNOWN";
    }
}

const char* merope::to_string(semantic_type_t type) noexcept {
    switch (type) {
    case semantic_type_t::identifier: return "IDENTIFIER";
    case semantic_type_t::quantity:   return "QUANTITY";
    case semantic_type_t::monetary:   return "MONETARY";
    case semantic_type_t::percentage: return "PERCENTAGE";
    case semantic_type_t::country:    return "COUNTRY";
    case semantic_type_t::category:   return "CATEGORY";
    case semantic_type_t::status:     return "STATUS";
    case semantic_type_t::date_value: return "DATE";
    case semantic_type_t::timestamp:  return "TIMESTAMP";
    case semantic_type_t::text:       return "TEXT";
    case semantic_type_t::flag:       return "FLAG";
    case semantic_type_t::email:      return "EMAIL";
    case semantic_type_t::unknown:
    default:                          return "UNKNOWN";
    }
}

merope::data_type_t merope::data_type_from_string(const std::string& name) noexcept {
    const std::string key = lower_copy(name);
    if (key == "int64" || key == "int" || key == "integer")    return data_type_t::int64;
    if (key == "float64" || key == "float" || key == "double") return data_type_t::float64;
    if (key == "bool" || key == "boolean")                     return data_type_t::boolean;
    if (key == "utf8" || key == "string" || key == "text")     return data_type_t::utf8;
    if (key == "categorical" || key == "category")             return data_type_t::categorical;
    if (key == "date")                                         return data_type_t::date;
    if (key == "datetime" || key == "timestamp")               return data_type_t::datetime;
    if (key == "decimal" || key == "money")                    return data_type_t::decimal;
    return data_type_t::unknown;
}

merope::semantic_type_t merope::semantic_type_from_string(const std::string& name) noexcept {
    const std::string key = lower_copy(name);
    if (key == "identifier" || key == "id")      return semantic_type_t::identifier;
    if (key == "quantity")                       return semantic_type_t::quantity;
    if (key == "monetary" || key == "money")     return semantic_type_t::monetary;
    if (key == "percentage" || key == "percent") return semantic_type_t::percentage;
    if (key == "country")                        return semantic_type_t::country;
    if (key == "category")                       return semantic_type_t::category;
    if (key == "status")                         return semantic_type_t::status;
    if (key == "date")                           return semantic_type_t::date_value;
    if (key == "timestamp" || key == "datetime") return semantic_type_t::timestamp;
    if (key == "text")                           return semantic_type_t::text;
    if (key == "flag" || key == "bool")          return semantic_type_t::flag;
    if (key == "email")                          return semantic_type_t::email;
    return semantic_type_t::unknown;
}

bool merope::is_null(const cell_value_t& value) noexcept {
    return std::holds_alternative<std::monostate>(value);
}

std::string merope::cell_to_display(const cell_value_t& value, data_type_t type) {
    if (is_null(value)) return "NULL";

    char buffer[64];
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    if (const auto* flag = std::get_if<bool>(&value)) {
        return *flag ? "true" : "false";
    }
    if (const auto* real = std::get_if<double>(&value)) {
        std::snprintf(buffer, sizeof(buffer), "%.4f", *real);
        return buffer;
    }

    const std::int64_t raw = std::get<std::int64_t>(value);
    switch (type) {
    case data_type_t::decimal: {
        const bool         negative  = raw < 0;
        const std::int64_t magnitude = negative ? -raw : raw;
        std::snprintf(buffer, sizeof(buffer), "%s%lld.%04lld", negative ? "-" : "",
                      static_cast<long long>(magnitude / k_money_factor),
                      static_cast<long long>(magnitude % k_money_factor));
        return buffer;
    }
    case data_type_t::date: {
        int year = 0; unsigned month = 0, day = 0;
        civil_from_days(raw, year, month, day);
        std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u", year, month, day);
        return buffer;
    }
    case data_type_t::datetime: {
        const std::int64_t days = raw >= 0 ? raw / 86400 : (raw - 86399) / 86400;
        const std::int64_t secs = raw - days * 86400;
        int year = 0; unsigned month = 0, day = 0;
        civil_from_days(days, year, month, day);
        std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u %02lld:%02lld:%02lld", year, month, day,
                      static_cast<long long>(secs / 3600),
                      static_cast<long long>((secs / 60) % 60),
                      static_cast<long long>(secs % 60));
        return buffer;
    }
    case data_type_t::boolean:
        return raw != 0 ? "true" : "false";
    default:
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(raw));
        return buffer;
    }
}
