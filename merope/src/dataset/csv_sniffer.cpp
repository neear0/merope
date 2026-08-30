#include "csv_sniffer.h"

#include "../core/parse.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

static constexpr char        k_delimiter_candidates[] = {',', ';', '\t', '|'};
static constexpr std::size_t k_scored_lines           = 60;
static constexpr std::size_t k_typed_probe_rows       = 200;

static std::string read_head(const std::string& path, std::size_t max_bytes, std::uint64_t& file_size) {
    std::error_code ec;
    file_size = std::filesystem::file_size(path, ec);
    if (ec) file_size = 0;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open dataset: " + path);

    std::string buffer(max_bytes, '\0');
    stream.read(buffer.data(), static_cast<std::streamsize>(max_bytes));
    buffer.resize(static_cast<std::size_t>(stream.gcount()));
    return buffer;
}

// Splits on physical newlines only. Good enough for scoring candidates: a
// delimiter that is wrong will look inconsistent regardless of quoting.
static std::vector<std::string_view> physical_lines(std::string_view text, std::size_t limit) {
    std::vector<std::string_view> lines;
    std::size_t begin = 0;
    while (begin < text.size() && lines.size() < limit) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string_view::npos) break;  // drop the trailing partial line
        std::size_t stop = end;
        if (stop > begin && text[stop - 1] == '\r') --stop;
        if (stop > begin) lines.push_back(text.substr(begin, stop - begin));
        begin = end + 1;
    }
    return lines;
}

static merope::delimiter_score_t score_delimiter(const std::vector<std::string_view>& lines, char delimiter) {
    merope::csv_dialect_t probe;
    probe.delimiter = delimiter;

    std::map<std::size_t, std::size_t> histogram;
    for (const std::string_view line : lines) {
        histogram[merope::split_line(line, probe).size()]++;
    }

    merope::delimiter_score_t result;
    result.delimiter = delimiter;
    if (histogram.empty()) return result;

    std::size_t best_count = 0;
    std::size_t best_hits  = 0;
    for (const auto& [count, hits] : histogram) {
        if (hits > best_hits || (hits == best_hits && count > best_count)) {
            best_hits  = hits;
            best_count = count;
        }
    }
    result.modal_count = best_count;
    result.consistency = static_cast<double>(best_hits) / static_cast<double>(lines.size());
    // A delimiter that never splits anything is not a delimiter.
    if (best_count < 2) return result;
    // Reward consistency first, then the number of columns it explains, but
    // stop rewarding width past a point so noisy text does not win.
    result.score = result.consistency * (1.0 + std::min<double>(static_cast<double>(best_count), 24.0) / 24.0);
    return result;
}

static bool is_number_ish(std::string_view field) noexcept {
    if (field.empty()) return false;
    bool saw_digit = false;
    for (const char ch : field) {
        if (std::isdigit(static_cast<unsigned char>(ch)) != 0) { saw_digit = true; continue; }
        if (ch == '.' || ch == ',' || ch == ' ' || ch == '-' || ch == '+') continue;
        return false;
    }
    return saw_digit;
}

// Votes for decimal and thousands separators by looking at where the digit
// groups fall. Ambiguous shapes such as "1.234" abstain.
static void vote_number_format(std::string_view field, std::map<char, int>& decimal_votes,
                        std::map<char, int>& thousands_votes) {
    if (!is_number_ish(field)) return;

    struct separator_t { char ch; std::size_t group_after; };
    std::vector<separator_t> separators;
    std::size_t index = 0;
    while (index < field.size()) {
        const char ch = field[index];
        if (ch == '.' || ch == ',' || ch == ' ') {
            std::size_t digits = 0;
            std::size_t look   = index + 1;
            while (look < field.size() && std::isdigit(static_cast<unsigned char>(field[look])) != 0) {
                ++digits;
                ++look;
            }
            separators.push_back({ch, digits});
            index = look;
            continue;
        }
        ++index;
    }
    if (separators.empty()) return;

    if (separators.size() == 1) {
        const separator_t& only = separators.front();
        if (only.ch == ' ') {
            if (only.group_after == 3) thousands_votes[' ']++;
            return;
        }
        if (only.group_after == 3) return;             // ambiguous, abstain
        if (only.group_after >= 1 && only.group_after <= 6) decimal_votes[only.ch]++;
        return;
    }

    // Several separators: the repeated one grouping threes is the thousands
    // separator, and a different trailing one is the decimal separator.
    const separator_t& last = separators.back();
    bool leading_all_threes = true;
    char grouping_char      = separators.front().ch;
    for (std::size_t k = 0; k + 1 < separators.size(); ++k) {
        if (separators[k].group_after != 3 || separators[k].ch != grouping_char) {
            leading_all_threes = false;
            break;
        }
    }
    if (!leading_all_threes) return;

    if (last.ch == grouping_char) {
        if (last.group_after == 3) thousands_votes[grouping_char]++;
        return;
    }
    thousands_votes[grouping_char]++;
    if (last.group_after >= 1 && last.group_after <= 6) decimal_votes[last.ch]++;
}

static bool parses_as_typed(std::string_view field, const merope::parse_options_t& options) {
    std::int64_t   as_int = 0;
    double         as_real = 0.0;
    bool           as_bool = false;
    merope::date_pattern_t pattern = merope::date_pattern_t::none;
    if (merope::looks_like_null(field)) return false;
    if (merope::parse_int64(field, options, as_int)) return true;
    if (merope::parse_float64(field, options, as_real)) return true;
    if (merope::parse_bool(field, as_bool)) return true;
    if (merope::parse_datetime(field, as_int, pattern)) return true;
    return false;
}

merope::sniff_result_t merope::sniff_csv(const std::string& path, std::size_t sample_bytes) {
    sniff_result_t result;
    std::string head = read_head(path, sample_bytes, result.file_size_bytes);
    result.sample_bytes = head.size();
    if (head.empty()) throw std::runtime_error("dataset is empty: " + path);

    // 1. Encoding, before anything tries to read characters.
    csv_dialect_t& dialect = result.dialect;
    dialect.encoding = detect_encoding(head.data(), head.size());
    result.notes.push_back(std::string("encoding: ") + to_string(dialect.encoding));
    if (!is_byte_oriented(dialect.encoding)) {
        throw std::runtime_error(std::string("unsupported encoding for streaming: ") +
                                 to_string(dialect.encoding));
    }
    head.erase(0, bom_length(dialect.encoding));
    if (dialect.encoding == encoding_t::windows_1250 || dialect.encoding == encoding_t::iso_8859_2) {
        head = transcode_to_utf8(head, dialect.encoding);
    }

    dialect.crlf = head.find("\r\n") != std::string::npos;

    // 2. Delimiter.
    const std::vector<std::string_view> lines = physical_lines(head, k_scored_lines);
    if (lines.empty()) throw std::runtime_error("dataset has no complete line in the sample: " + path);

    delimiter_score_t best;
    for (const char candidate : k_delimiter_candidates) {
        const delimiter_score_t scored = score_delimiter(lines, candidate);
        if (scored.score > best.score) best = scored;
    }
    if (best.score <= 0.0) {
        // Single column file. Still perfectly valid.
        best.delimiter   = ',';
        best.modal_count = 1;
        best.consistency = 1.0;
        result.notes.push_back("delimiter: none found, treating the file as single column");
    }
    dialect.delimiter            = best.delimiter;
    dialect.column_count         = std::max<std::size_t>(best.modal_count, 1);
    dialect.delimiter_confidence = best.consistency;
    result.notes.push_back("delimiter: " + std::string(1, dialect.delimiter == '\t' ? 't' : dialect.delimiter) +
                           (dialect.delimiter == '\t' ? " (tab)" : "") +
                           ", consistency " + std::to_string(best.consistency));

    // 3. Re-split properly with quoting, and find out whether quoted fields
    //    contain newlines. That single fact decides if we may partition.
    std::vector<std::vector<std::string>> records;
    std::vector<std::string>              fields;
    std::size_t                           cursor = 0;
    while (records.size() < k_typed_probe_rows) {
        const std::size_t before = cursor;
        const record_status_t status = parse_record(head, cursor, dialect, fields);
        if (status != record_status_t::ok) break;
        // The record terminator sits at the very end of the consumed span, so
        // any line break before it can only have come from inside a quoted field.
        std::string_view consumed(head.data() + before, cursor - before);
        while (!consumed.empty() && (consumed.back() == '\n' || consumed.back() == '\r')) {
            consumed.remove_suffix(1);
        }
        if (consumed.find_first_of("\r\n") != std::string_view::npos) {
            dialect.quoted_newlines = true;
        }
        records.push_back(fields);
    }
    if (records.empty()) throw std::runtime_error("cannot parse any record from: " + path);
    if (dialect.quoted_newlines) {
        result.notes.push_back("quoted newlines present: byte range partitioning disabled");
    }

    // 4. Number formatting, voted on the body rows only.
    std::map<char, int> decimal_votes;
    std::map<char, int> thousands_votes;
    for (std::size_t row = records.size() > 1 ? 1 : 0; row < records.size(); ++row) {
        for (const std::string& field : records[row]) {
            vote_number_format(trim(field), decimal_votes, thousands_votes);
        }
    }
    auto winner = [](const std::map<char, int>& votes, char fallback) {
        char  best_char = fallback;
        int   best_hits = 0;
        for (const auto& [ch, hits] : votes) {
            if (hits > best_hits) { best_hits = hits; best_char = ch; }
        }
        return best_hits > 0 ? best_char : fallback;
    };
    dialect.decimal_separator = winner(decimal_votes, '.');
    // A separator cannot be both, and it certainly cannot be the delimiter.
    dialect.thousands_separator = winner(thousands_votes, '\0');
    if (dialect.thousands_separator == dialect.decimal_separator ||
        dialect.thousands_separator == dialect.delimiter) {
        dialect.thousands_separator = '\0';
    }
    if (dialect.decimal_separator == dialect.delimiter) dialect.decimal_separator = '.';
    result.notes.push_back(std::string("decimal separator: ") + dialect.decimal_separator +
                           (dialect.thousands_separator != '\0'
                                ? std::string(", thousands separator: ") + dialect.thousands_separator
                                : std::string(", no thousands separator")));

    // 5. Header presence. A header row is text where the body is typed.
    parse_options_t options;
    options.decimal_separator   = dialect.decimal_separator;
    options.thousands_separator = dialect.thousands_separator;

    const std::vector<std::string>& first = records.front();
    double typed_columns  = 0.0;
    double header_columns = 0.0;
    if (records.size() >= 3) {
        for (std::size_t column = 0; column < first.size(); ++column) {
            std::size_t typed_body = 0;
            std::size_t body_rows  = 0;
            for (std::size_t row = 1; row < records.size(); ++row) {
                if (column >= records[row].size()) continue;
                ++body_rows;
                if (parses_as_typed(records[row][column], options)) ++typed_body;
            }
            if (body_rows == 0) continue;
            const double typed_ratio = static_cast<double>(typed_body) / static_cast<double>(body_rows);
            if (typed_ratio < 0.8) continue;  // a text column tells us nothing
            typed_columns += 1.0;
            if (!parses_as_typed(first[column], options)) header_columns += 1.0;
        }
    }

    if (typed_columns > 0.0) {
        dialect.header_confidence = header_columns / typed_columns;
        dialect.has_header        = dialect.header_confidence >= 0.5;
        result.notes.push_back("header: decided from type contrast on " +
                               std::to_string(static_cast<int>(typed_columns)) + " typed column(s)");
    } else {
        // Everything is text. Fall back to the weaker signal: a header row has
        // distinct, non empty labels.
        std::vector<std::string> sorted = first;
        std::sort(sorted.begin(), sorted.end());
        const bool unique_labels = std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
        const bool non_empty     = std::none_of(first.begin(), first.end(),
                                                [](const std::string& f) { return trim(f).empty(); });
        dialect.has_header        = unique_labels && non_empty && records.size() > 1;
        dialect.header_confidence = dialect.has_header ? 0.5 : 0.0;
        result.notes.push_back("header: no typed column to contrast against, used label uniqueness");
    }

    if (dialect.column_count == 0) dialect.column_count = std::max<std::size_t>(first.size(), 1);
    dialect.column_names.clear();
    std::set<std::string> taken;
    for (std::size_t column = 0; column < dialect.column_count; ++column) {
        std::string name = dialect.has_header && column < first.size()
                               ? std::string(trim(first[column]))
                               : std::string();
        if (name.empty()) name = "column_" + std::to_string(column);
        if (taken.count(name) != 0) {
            const std::string base = name;
            std::size_t       next = 2;
            do {
                name = base + "_" + std::to_string(next++);
            } while (taken.count(name) != 0);
        }
        taken.insert(name);
        dialect.column_names.push_back(std::move(name));
    }

    return result;
}

