#include "csv_format.h"

merope::record_status_t merope::parse_record(std::string_view buffer, std::size_t& cursor,
                                               const csv_dialect_t& dialect,
                                               std::vector<std::string>& fields) {
    fields.clear();
    if (cursor >= buffer.size()) return record_status_t::end;

    const std::size_t start = cursor;
    std::size_t index = cursor;
    std::string field;
    bool in_quotes  = false;
    bool terminated = false;

    while (index < buffer.size()) {
        const char ch = buffer[index];

        if (in_quotes) {
            if (ch == dialect.quote) {
                // A doubled quote inside a quoted field is a literal quote.
                if (index + 1 < buffer.size() && buffer[index + 1] == dialect.quote) {
                    field.push_back(dialect.quote);
                    index += 2;
                    continue;
                }
                if (index + 1 == buffer.size()) {
                    // Cannot tell yet whether this closes the field or escapes
                    // the next quote. Ask for more input.
                    cursor = start;
                    return record_status_t::incomplete;
                }
                in_quotes = false;
                ++index;
                continue;
            }
            field.push_back(ch);
            ++index;
            continue;
        }

        if (ch == dialect.quote && field.empty()) {
            in_quotes = true;
            ++index;
            continue;
        }
        if (ch == dialect.delimiter) {
            fields.push_back(field);
            field.clear();
            ++index;
            continue;
        }
        if (ch == '\n' || ch == '\r') {
            ++index;
            if (ch == '\r' && index < buffer.size() && buffer[index] == '\n') ++index;
            terminated = true;
            break;
        }
        field.push_back(ch);
        ++index;
    }

    if (in_quotes) {
        // The record runs past the end of what we have.
        cursor = start;
        return record_status_t::incomplete;
    }
    if (!terminated && index >= buffer.size()) {
        // Last line of the buffer with no terminator. The caller decides
        // whether that is the end of the file or a short read.
        cursor = start;
        return record_status_t::incomplete;
    }

    fields.push_back(field);
    cursor = index;
    return record_status_t::ok;
}

std::vector<std::string> merope::split_line(std::string_view line, const csv_dialect_t& dialect) {
    std::string padded(line);
    padded.push_back('\n');

    std::vector<std::string> fields;
    std::size_t cursor = 0;
    parse_record(padded, cursor, dialect, fields);
    return fields;
}

