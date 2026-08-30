// dataset/csv_format.h - the dialect a CSV file turned out to be written in,
// plus the record splitter that every other stage goes through.
#pragma once

#include "encoding.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace merope {

struct csv_dialect_t {
    char        delimiter           = ',';
    char        quote               = '"';
    bool        has_header          = false;
    encoding_t  encoding            = encoding_t::utf8;
    char        decimal_separator   = '.';
    char        thousands_separator = '\0';
    bool        crlf                = false;

    // Set when a quoted field was seen to contain a line break. Byte range
    // partitioning cuts on newlines, so this forces single partition reads.
    bool        quoted_newlines     = false;

    std::size_t column_count        = 0;
    double      delimiter_confidence = 0.0;
    double      header_confidence    = 0.0;

    // Physical column names: the header row if there was one, otherwise
    // generated column_0, column_1, ...
    std::vector<std::string> column_names;
};

enum class record_status_t : std::uint8_t {
    ok,          // a complete record was produced
    incomplete,  // buffer ended mid record; caller should refill and retry
    end          // nothing left in the buffer
};

record_status_t parse_record(std::string_view buffer, std::size_t& cursor,
                             const csv_dialect_t& dialect,
                             std::vector<std::string>& fields);

// Convenience wrapper for a single in-memory line.
std::vector<std::string> split_line(std::string_view line, const csv_dialect_t& dialect);

}
