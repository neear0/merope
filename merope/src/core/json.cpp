#include "json.h"

#include "json_parser.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

static void append_utf8_code_point(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

static void escape_into(const std::string& text, std::string& out) {
    out.push_back('"');
    for (const char ch : text) {
        switch (ch) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(ch));
                out += buffer;
            } else {
                out.push_back(ch);  // UTF-8 bytes pass through unescaped
            }
        }
    }
    out.push_back('"');
}

static void serialize_into(const merope::json_value_t& value, std::string& out, int indent, int depth) {
    const std::string pad     = indent > 0 ? std::string(static_cast<std::size_t>(indent * (depth + 1)), ' ') : std::string();
    const std::string pad_end = indent > 0 ? std::string(static_cast<std::size_t>(indent * depth), ' ') : std::string();
    const char*       newline = indent > 0 ? "\n" : "";

    switch (value.kind) {
    case merope::json_kind_t::null:    out += "null"; break;
    case merope::json_kind_t::boolean: out += value.boolean_value ? "true" : "false"; break;
    case merope::json_kind_t::number: {
        char buffer[40];
        const double rounded = value.number_value;
        if (std::isfinite(rounded) && rounded == std::floor(rounded) &&
            std::fabs(rounded) < 9.0e15) {
            std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(rounded));
        } else {
            std::snprintf(buffer, sizeof(buffer), "%.10g", rounded);
        }
        out += buffer;
        break;
    }
    case merope::json_kind_t::string: escape_into(value.string_value, out); break;
    case merope::json_kind_t::array:
        if (value.array_value.empty()) { out += "[]"; break; }
        out += "[";
        out += newline;
        for (std::size_t index = 0; index < value.array_value.size(); ++index) {
            out += pad;
            serialize_into(value.array_value[index], out, indent, depth + 1);
            if (index + 1 < value.array_value.size()) out += ",";
            out += newline;
        }
        out += pad_end;
        out += "]";
        break;
    case merope::json_kind_t::object:
        if (value.object_value.empty()) { out += "{}"; break; }
        out += "{";
        out += newline;
        for (std::size_t index = 0; index < value.object_value.size(); ++index) {
            out += pad;
            escape_into(value.object_value[index].first, out);
            out += indent > 0 ? ": " : ":";
            serialize_into(value.object_value[index].second, out, indent, depth + 1);
            if (index + 1 < value.object_value.size()) out += ",";
            out += newline;
        }
        out += pad_end;
        out += "}";
        break;
    }
}

merope::c_json_parser::c_json_parser(std::string_view text, std::string& error)
    : m_text(text), m_error(error) {}

bool merope::c_json_parser::run(json_value_t& out) {
    skip_space();
    if (!parse_value(out, 0)) return false;
    skip_space();
    if (m_cursor != m_text.size()) return fail("trailing content after the JSON value");
    return true;
}

bool merope::c_json_parser::fail(const char* message) {
    m_error = std::string(message) + " at offset " + std::to_string(m_cursor);
    return false;
}

void merope::c_json_parser::skip_space() {
    while (m_cursor < m_text.size()) {
        const char ch = m_text[m_cursor];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') { ++m_cursor; continue; }
        break;
    }
}

bool merope::c_json_parser::literal(std::string_view word) {
    if (m_text.compare(m_cursor, word.size(), word) != 0) return false;
    m_cursor += word.size();
    return true;
}

bool merope::c_json_parser::parse_value(json_value_t& out, int depth) {
    if (depth > k_max_depth) return fail("nesting too deep");
    if (m_cursor >= m_text.size()) return fail("unexpected end of input");

    switch (m_text[m_cursor]) {
    case '{': return parse_object(out, depth);
    case '[': return parse_array(out, depth);
    case '"': {
        std::string text;
        if (!parse_string(text)) return false;
        out = json_value_t::make_string(std::move(text));
        return true;
    }
    case 't':
        if (!literal("true")) return fail("invalid literal");
        out = json_value_t::make_bool(true);
        return true;
    case 'f':
        if (!literal("false")) return fail("invalid literal");
        out = json_value_t::make_bool(false);
        return true;
    case 'n':
        if (!literal("null")) return fail("invalid literal");
        out = json_value_t::make_null();
        return true;
    default:
        return parse_number(out);
    }
}

bool merope::c_json_parser::parse_object(json_value_t& out, int depth) {
    ++m_cursor;  // '{'
    out = json_value_t::make_object();
    skip_space();
    if (m_cursor < m_text.size() && m_text[m_cursor] == '}') { ++m_cursor; return true; }

    while (true) {
        skip_space();
        std::string key;
        if (!parse_string(key)) return false;
        skip_space();
        if (m_cursor >= m_text.size() || m_text[m_cursor] != ':') return fail("expected :");
        ++m_cursor;
        skip_space();
        json_value_t value;
        if (!parse_value(value, depth + 1)) return false;
        out.object_value.emplace_back(std::move(key), std::move(value));
        skip_space();
        if (m_cursor >= m_text.size()) return fail("unterminated object");
        if (m_text[m_cursor] == ',') { ++m_cursor; continue; }
        if (m_text[m_cursor] == '}') { ++m_cursor; return true; }
        return fail("expected , or } in object");
    }
}

bool merope::c_json_parser::parse_array(json_value_t& out, int depth) {
    ++m_cursor;  // '['
    out = json_value_t::make_array();
    skip_space();
    if (m_cursor < m_text.size() && m_text[m_cursor] == ']') { ++m_cursor; return true; }

    while (true) {
        skip_space();
        json_value_t value;
        if (!parse_value(value, depth + 1)) return false;
        out.array_value.push_back(std::move(value));
        skip_space();
        if (m_cursor >= m_text.size()) return fail("unterminated array");
        if (m_text[m_cursor] == ',') { ++m_cursor; continue; }
        if (m_text[m_cursor] == ']') { ++m_cursor; return true; }
        return fail("expected , or ] in array");
    }
}

bool merope::c_json_parser::parse_hex4(std::uint32_t& out) {
    if (m_cursor + 4 > m_text.size()) return false;
    out = 0;
    for (int k = 0; k < 4; ++k) {
        const auto ch = static_cast<unsigned char>(m_text[m_cursor + static_cast<std::size_t>(k)]);
        std::uint32_t digit = 0;
        if (ch >= '0' && ch <= '9')      digit = static_cast<std::uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') digit = static_cast<std::uint32_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') digit = static_cast<std::uint32_t>(ch - 'A' + 10);
        else return false;
        out = (out << 4) | digit;
    }
    m_cursor += 4;
    return true;
}

bool merope::c_json_parser::parse_string(std::string& out) {
    if (m_cursor >= m_text.size() || m_text[m_cursor] != '"') return fail("expected a string");
    ++m_cursor;
    out.clear();
    while (m_cursor < m_text.size()) {
        const char ch = m_text[m_cursor];
        if (ch == '"') { ++m_cursor; return true; }
        if (ch != '\\') { out.push_back(ch); ++m_cursor; continue; }

        ++m_cursor;
        if (m_cursor >= m_text.size()) return fail("unterminated escape");
        const char escape = m_text[m_cursor++];
        switch (escape) {
        case '"':  out.push_back('"');  break;
        case '\\': out.push_back('\\'); break;
        case '/':  out.push_back('/');  break;
        case 'b':  out.push_back('\b'); break;
        case 'f':  out.push_back('\f'); break;
        case 'n':  out.push_back('\n'); break;
        case 'r':  out.push_back('\r'); break;
        case 't':  out.push_back('\t'); break;
        case 'u': {
            std::uint32_t code = 0;
            if (!parse_hex4(code)) return fail("bad \\u escape");
            if (code >= 0xD800 && code <= 0xDBFF) {
                // Surrogate pair; the low half must follow.
                if (m_cursor + 1 < m_text.size() && m_text[m_cursor] == '\\' &&
                    m_text[m_cursor + 1] == 'u') {
                    m_cursor += 2;
                    std::uint32_t low = 0;
                    if (!parse_hex4(low)) return fail("bad \\u escape");
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    } else {
                        code = 0xFFFD;
                    }
                } else {
                    code = 0xFFFD;
                }
            } else if (code >= 0xDC00 && code <= 0xDFFF) {
                code = 0xFFFD;
            }
            append_utf8_code_point(out, code);
            break;
        }
        default:
            return fail("unknown escape");
        }
    }
    return fail("unterminated string");
}

bool merope::c_json_parser::parse_number(json_value_t& out) {
    const std::size_t start = m_cursor;
    if (m_cursor < m_text.size() && (m_text[m_cursor] == '-' || m_text[m_cursor] == '+')) ++m_cursor;
    bool saw_digit = false;
    while (m_cursor < m_text.size() &&
           std::isdigit(static_cast<unsigned char>(m_text[m_cursor])) != 0) {
        saw_digit = true;
        ++m_cursor;
    }
    if (m_cursor < m_text.size() && m_text[m_cursor] == '.') {
        ++m_cursor;
        while (m_cursor < m_text.size() &&
               std::isdigit(static_cast<unsigned char>(m_text[m_cursor])) != 0) {
            saw_digit = true;
            ++m_cursor;
        }
    }
    if (saw_digit && m_cursor < m_text.size() && (m_text[m_cursor] == 'e' || m_text[m_cursor] == 'E')) {
        ++m_cursor;
        if (m_cursor < m_text.size() && (m_text[m_cursor] == '-' || m_text[m_cursor] == '+')) ++m_cursor;
        while (m_cursor < m_text.size() &&
               std::isdigit(static_cast<unsigned char>(m_text[m_cursor])) != 0) {
            ++m_cursor;
        }
    }
    if (!saw_digit) return fail("expected a value");

    const std::string digits(m_text.substr(start, m_cursor - start));
    out = json_value_t::make_number(std::strtod(digits.c_str(), nullptr));
    return true;
}

merope::json_value_t merope::json_value_t::make_null() { return json_value_t{}; }

merope::json_value_t merope::json_value_t::make_bool(bool value) {
    json_value_t out;
    out.kind = json_kind_t::boolean;
    out.boolean_value = value;
    return out;
}

merope::json_value_t merope::json_value_t::make_number(double value) {
    json_value_t out;
    out.kind = json_kind_t::number;
    out.number_value = value;
    return out;
}

merope::json_value_t merope::json_value_t::make_string(std::string value) {
    json_value_t out;
    out.kind = json_kind_t::string;
    out.string_value = std::move(value);
    return out;
}

merope::json_value_t merope::json_value_t::make_array(json_array_t value) {
    json_value_t out;
    out.kind = json_kind_t::array;
    out.array_value = std::move(value);
    return out;
}

merope::json_value_t merope::json_value_t::make_object(json_object_t value) {
    json_value_t out;
    out.kind = json_kind_t::object;
    out.object_value = std::move(value);
    return out;
}

const merope::json_value_t* merope::json_value_t::find(std::string_view key) const noexcept {
    if (kind != json_kind_t::object) return nullptr;
    for (const auto& member : object_value) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

std::string merope::json_value_t::string_or(std::string_view key, std::string fallback) const {
    const json_value_t* member = find(key);
    return member != nullptr && member->is_string() ? member->string_value : fallback;
}

double merope::json_value_t::number_or(std::string_view key, double fallback) const noexcept {
    const json_value_t* member = find(key);
    return member != nullptr && member->is_number() ? member->number_value : fallback;
}

bool merope::json_value_t::bool_or(std::string_view key, bool fallback) const noexcept {
    const json_value_t* member = find(key);
    return member != nullptr && member->is_bool() ? member->boolean_value : fallback;
}

std::int64_t merope::json_value_t::int_or(std::string_view key, std::int64_t fallback) const noexcept {
    const json_value_t* member = find(key);
    if (member == nullptr || !member->is_number()) return fallback;
    return static_cast<std::int64_t>(member->number_value);
}

void merope::json_value_t::set(std::string key, json_value_t value) {
    if (kind != json_kind_t::object) {
        kind = json_kind_t::object;
        object_value.clear();
    }
    for (auto& member : object_value) {
        if (member.first == key) { member.second = std::move(value); return; }
    }
    object_value.emplace_back(std::move(key), std::move(value));
}

bool merope::json_parse(std::string_view text, json_value_t& out, std::string& error) {
    error.clear();
    c_json_parser parser(text, error);
    return parser.run(out);
}

std::string merope::json_serialize(const json_value_t& value, int indent) {
    std::string out;
    serialize_into(value, out, indent, 0);
    return out;
}
