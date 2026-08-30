#include "parse.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

static bool equals_ignore_case(std::string_view text, std::string_view other) noexcept {
    if (text.size() != other.size()) return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto left  = static_cast<unsigned char>(text[index]);
        const auto right = static_cast<unsigned char>(other[index]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
}

static bool read_uint(std::string_view text, std::size_t& index, std::size_t digits, int& out) noexcept {
    if (index + digits > text.size()) return false;
    int value = 0;
    for (std::size_t k = 0; k < digits; ++k) {
        const auto ch = static_cast<unsigned char>(text[index + k]);
        if (std::isdigit(ch) == 0) return false;
        value = value * 10 + (ch - '0');
    }
    index += digits;
    out = value;
    return true;
}

static bool valid_civil(int year, int month, int day) noexcept {
    if (year < 1000 || year > 9999) return false;
    if (month < 1 || month > 12) return false;
    if (day < 1) return false;
    static constexpr int k_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int limit = k_days[month - 1];
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        if (leap) limit = 29;
    }
    return day <= limit;
}

// Strips grouping separators and normalises the decimal separator to a dot.
// Returns false when the text contains anything a number cannot contain.
static bool normalise_number(std::string_view text, const merope::parse_options_t& options,
                             std::string& out, bool& saw_fraction, int& fraction_digits) noexcept {
    out.clear();
    saw_fraction    = false;
    fraction_digits = 0;

    bool saw_digit    = false;
    bool saw_exponent = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char ch = text[index];
        if (ch == options.thousands_separator && options.thousands_separator != '\0') {
            continue;
        }
        if (ch == options.decimal_separator) {
            if (saw_fraction || saw_exponent) return false;
            saw_fraction = true;
            out.push_back('.');
            continue;
        }
        if (ch == '+' || ch == '-') {
            // Sign is only legal leading, or straight after an exponent marker.
            const bool leading = out.empty();
            const bool after_e = !out.empty() && (out.back() == 'e' || out.back() == 'E');
            if (!leading && !after_e) return false;
            out.push_back(ch);
            continue;
        }
        if (ch == 'e' || ch == 'E') {
            if (!saw_digit || saw_exponent) return false;
            saw_exponent = true;
            out.push_back('e');
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) return false;
        if (saw_fraction && !saw_exponent) ++fraction_digits;
        saw_digit = true;
        out.push_back(ch);
    }
    return saw_digit;
}

const char* merope::to_string(date_pattern_t pattern) noexcept {
    switch (pattern) {
    case date_pattern_t::iso:       return "YYYY-MM-DD";
    case date_pattern_t::iso_slash: return "YYYY/MM/DD";
    case date_pattern_t::dmy_dot:   return "DD.MM.YYYY";
    case date_pattern_t::dmy_slash: return "DD/MM/YYYY";
    case date_pattern_t::mdy_slash: return "MM/DD/YYYY";
    case date_pattern_t::none:
    default:                        return "-";
    }
}

std::string_view merope::trim(std::string_view text) noexcept {
    std::size_t begin = 0;
    std::size_t end   = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
    return text.substr(begin, end - begin);
}

bool merope::looks_like_null(std::string_view text, bool text_column) noexcept {
    const std::string_view value = trim(text);
    if (value.empty()) return true;
    // Never a legitimate value, whatever the column holds.
    static constexpr std::string_view k_unambiguous[] = {
        "null", "n/a", "nan", "\\n"
    };
    for (const std::string_view token : k_unambiguous) {
        if (equals_ignore_case(value, token)) return true;
    }
    if (text_column) return false;
    // Missing markers in a numeric, date or boolean column; real values in a
    // text one, so they are only nulls when the column is not text.
    static constexpr std::string_view k_non_text[] = {
        "nil", "na", "none", "-", "--", "?"
    };
    for (const std::string_view token : k_non_text) {
        if (equals_ignore_case(value, token)) return true;
    }
    return false;
}

bool merope::parse_int64(std::string_view text, const parse_options_t& options, std::int64_t& out) noexcept {
    const std::string_view value = trim(text);
    if (value.empty()) return false;

    std::string normalised;
    bool saw_fraction = false;
    int  fraction_digits = 0;
    if (!normalise_number(value, options, normalised, saw_fraction, fraction_digits)) return false;
    if (saw_fraction) return false;
    if (normalised.find('e') != std::string::npos) return false;

    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(normalised.c_str(), &end, 10);
    if (errno == ERANGE || end == normalised.c_str() || *end != '\0') return false;
    out = static_cast<std::int64_t>(parsed);
    return true;
}

bool merope::parse_float64(std::string_view text, const parse_options_t& options, double& out) noexcept {
    const std::string_view value = trim(text);
    if (value.empty()) return false;

    std::string normalised;
    bool saw_fraction = false;
    int  fraction_digits = 0;
    if (!normalise_number(value, options, normalised, saw_fraction, fraction_digits)) return false;

    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(normalised.c_str(), &end);
    if (end == normalised.c_str() || *end != '\0') return false;
    if (!std::isfinite(parsed)) return false;
    out = parsed;
    return true;
}

bool merope::parse_bool(std::string_view text, bool& out) noexcept {
    const std::string_view value = trim(text);
    static constexpr std::string_view k_true[]  = {"true", "t", "yes", "y", "1", "ano"};
    static constexpr std::string_view k_false[] = {"false", "f", "no", "n", "0", "nie"};
    for (const std::string_view token : k_true) {
        if (equals_ignore_case(value, token)) { out = true; return true; }
    }
    for (const std::string_view token : k_false) {
        if (equals_ignore_case(value, token)) { out = false; return true; }
    }
    return false;
}

bool merope::parse_decimal(std::string_view text, const parse_options_t& options, std::int64_t& out) noexcept {
    std::string_view value = trim(text);
    if (value.empty()) return false;

    // Tolerate a currency symbol or code on either side.
    static constexpr std::string_view k_symbols[] = {"EUR", "USD", "CZK", "PLN", "GBP", "\xE2\x82\xAC", "$", "K\xC4\x8D"};
    for (const std::string_view symbol : k_symbols) {
        if (value.size() > symbol.size() && equals_ignore_case(value.substr(0, symbol.size()), symbol)) {
            value = trim(value.substr(symbol.size()));
            break;
        }
        if (value.size() > symbol.size() &&
            equals_ignore_case(value.substr(value.size() - symbol.size()), symbol)) {
            value = trim(value.substr(0, value.size() - symbol.size()));
            break;
        }
    }
    if (value.empty()) return false;

    std::string normalised;
    bool saw_fraction    = false;
    int  fraction_digits = 0;
    if (!normalise_number(value, options, normalised, saw_fraction, fraction_digits)) return false;
    if (normalised.find('e') != std::string::npos) return false;
    // Refusing to round here is deliberate: silently dropping fraction digits
    // is exactly the money bug the spec calls out.
    if (fraction_digits > k_money_scale) return false;

    const bool negative = normalised.front() == '-';
    if (normalised.front() == '+' || normalised.front() == '-') normalised.erase(0, 1);

    const std::size_t dot = normalised.find('.');
    std::string whole = dot == std::string::npos ? normalised : normalised.substr(0, dot);
    std::string frac  = dot == std::string::npos ? std::string() : normalised.substr(dot + 1);
    if (whole.empty()) whole = "0";
    frac.append(static_cast<std::size_t>(k_money_scale) - frac.size(), '0');

    errno = 0;
    char* end = nullptr;
    const long long units = std::strtoll(whole.c_str(), &end, 10);
    if (errno == ERANGE || *end != '\0') return false;
    const long long fraction = frac.empty() ? 0 : std::strtoll(frac.c_str(), &end, 10);
    if (*end != '\0') return false;

    const std::int64_t scaled = static_cast<std::int64_t>(units) * k_money_factor +
                                static_cast<std::int64_t>(fraction);
    out = negative ? -scaled : scaled;
    return true;
}

bool merope::parse_date(std::string_view text, std::int64_t& out_days, date_pattern_t& pattern) noexcept {
    const std::string_view value = trim(text);
    if (value.size() != 10) return false;

    int year = 0, month = 0, day = 0;
    std::size_t index = 0;

    if (value[4] == '-' || value[4] == '/') {
        const char sep = value[4];
        if (!read_uint(value, index, 4, year)) return false;
        ++index;
        if (!read_uint(value, index, 2, month)) return false;
        if (index >= value.size() || value[index] != sep) return false;
        ++index;
        if (!read_uint(value, index, 2, day)) return false;
        pattern = sep == '-' ? date_pattern_t::iso : date_pattern_t::iso_slash;
    } else if (value[2] == '.' || value[2] == '/') {
        const char sep = value[2];
        int first = 0, second = 0;
        if (!read_uint(value, index, 2, first)) return false;
        ++index;
        if (!read_uint(value, index, 2, second)) return false;
        if (index >= value.size() || value[index] != sep) return false;
        ++index;
        if (!read_uint(value, index, 4, year)) return false;
        if (sep == '.') {
            day = first; month = second;
            pattern = date_pattern_t::dmy_dot;
        } else if (first > 12) {
            day = first; month = second;
            pattern = date_pattern_t::dmy_slash;
        } else if (second > 12) {
            month = first; day = second;
            pattern = date_pattern_t::mdy_slash;
        } else {
            // Genuinely ambiguous. Central European convention wins, and the
            // profiler reports the pattern so a human can override it.
            day = first; month = second;
            pattern = date_pattern_t::dmy_slash;
        }
    } else {
        return false;
    }

    if (index != value.size()) return false;
    if (!valid_civil(year, month, day)) return false;
    out_days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    return true;
}

bool merope::parse_datetime(std::string_view text, std::int64_t& out_seconds, date_pattern_t& pattern) noexcept {
    std::string_view value = trim(text);
    if (value.size() < 10) return false;

    std::int64_t days = 0;
    if (!parse_date(value.substr(0, 10), days, pattern)) return false;
    if (value.size() == 10) {
        out_seconds = days * 86400;
        return true;
    }

    const char separator = value[10];
    if (separator != 'T' && separator != 't' && separator != ' ') return false;

    std::string_view time = value.substr(11);
    std::int64_t offset_seconds = 0;
    if (!time.empty() && (time.back() == 'Z' || time.back() == 'z')) {
        time.remove_suffix(1);
    } else if (time.size() > 6) {
        const char sign = time[time.size() - 6];
        if (sign == '+' || sign == '-') {
            std::string_view zone = time.substr(time.size() - 5);
            std::size_t cursor = 0;
            int zone_hours = 0, zone_minutes = 0;
            if (zone.size() == 5 && zone[2] == ':' &&
                read_uint(zone, cursor, 2, zone_hours)) {
                ++cursor;
                if (read_uint(zone, cursor, 2, zone_minutes)) {
                    offset_seconds = (zone_hours * 3600LL + zone_minutes * 60LL) * (sign == '-' ? 1 : -1);
                    time = time.substr(0, time.size() - 6);
                }
            }
        }
    }

    std::size_t cursor = 0;
    int hours = 0, minutes = 0, seconds = 0;
    if (!read_uint(time, cursor, 2, hours)) return false;
    if (cursor >= time.size() || time[cursor] != ':') return false;
    ++cursor;
    if (!read_uint(time, cursor, 2, minutes)) return false;
    if (cursor < time.size()) {
        if (time[cursor] != ':') return false;
        ++cursor;
        if (!read_uint(time, cursor, 2, seconds)) return false;
        // Fractional seconds are accepted and truncated.
        if (cursor < time.size() && time[cursor] == '.') {
            ++cursor;
            while (cursor < time.size() && std::isdigit(static_cast<unsigned char>(time[cursor])) != 0) ++cursor;
        }
    }
    if (cursor != time.size()) return false;
    if (hours > 23 || minutes > 59 || seconds > 60) return false;

    out_seconds = days * 86400 + hours * 3600LL + minutes * 60LL + seconds + offset_seconds;
    return true;
}

bool merope::looks_like_currency(std::string_view text) noexcept {
    const std::string_view value = trim(text);
    if (value.empty()) return false;
    static constexpr std::string_view k_symbols[] = {"EUR", "USD", "CZK", "PLN", "GBP", "\xE2\x82\xAC", "$"};
    for (const std::string_view symbol : k_symbols) {
        if (value.size() > symbol.size() &&
            (equals_ignore_case(value.substr(0, symbol.size()), symbol) ||
             equals_ignore_case(value.substr(value.size() - symbol.size()), symbol))) {
            return true;
        }
    }
    // Two fraction digits is the other classic monetary tell.
    const std::size_t dot = value.find_last_of(".,");
    if (dot == std::string_view::npos) return false;
    if (value.size() - dot - 1 != 2) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == dot) continue;
        const auto ch = static_cast<unsigned char>(value[index]);
        if (std::isdigit(ch) == 0 && ch != '-' && ch != '+') return false;
    }
    return true;
}

bool merope::looks_like_country_code(std::string_view text) noexcept {
    const std::string_view value = trim(text);
    if (value.size() != 2) return false;
    return std::isupper(static_cast<unsigned char>(value[0])) != 0 &&
           std::isupper(static_cast<unsigned char>(value[1])) != 0;
}

bool merope::looks_like_email(std::string_view text) noexcept {
    const std::string_view value = trim(text);
    const std::size_t at = value.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 >= value.size()) return false;
    const std::size_t dot = value.find('.', at);
    return dot != std::string_view::npos && dot + 1 < value.size() &&
           value.find('@', at + 1) == std::string_view::npos;
}

bool merope::looks_like_percentage(std::string_view text) noexcept {
    const std::string_view value = trim(text);
    return value.size() > 1 && value.back() == '%';
}

bool merope::parse_as(std::string_view text, data_type_t type, const parse_options_t& options,
                       std::int64_t& out_int, double& out_real, std::string& out_text) noexcept {
    date_pattern_t pattern = date_pattern_t::none;
    switch (type) {
    case data_type_t::int64:
        return parse_int64(text, options, out_int);
    case data_type_t::float64:
        return parse_float64(text, options, out_real);
    case data_type_t::decimal:
        return parse_decimal(text, options, out_int);
    case data_type_t::boolean: {
        bool flag = false;
        if (!parse_bool(text, flag)) return false;
        out_int = flag ? 1 : 0;
        return true;
    }
    case data_type_t::date:
        return parse_date(text, out_int, pattern);
    case data_type_t::datetime:
        return parse_datetime(text, out_int, pattern);
    case data_type_t::utf8:
    case data_type_t::categorical:
        out_text.assign(text.data(), text.size());
        return true;
    case data_type_t::unknown:
    default:
        return false;
    }
}

