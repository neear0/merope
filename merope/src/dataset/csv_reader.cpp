#include "csv_reader.h"

#include <filesystem>
#include <stdexcept>

static constexpr std::size_t k_max_quarantined = 100;

static std::string join_fields(const std::vector<std::string>& fields, char delimiter) {
    std::string out;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index > 0) out.push_back(delimiter);
        out += fields[index];
    }
    return out;
}

merope::bad_row_policy_t merope::bad_row_policy_from_string(const std::string& name) noexcept {
    if (name == "quarantine") return bad_row_policy_t::quarantine;
    if (name == "fail")       return bad_row_policy_t::fail;
    return bad_row_policy_t::skip;
}

const char* merope::to_string(bad_row_policy_t policy) noexcept {
    switch (policy) {
    case bad_row_policy_t::quarantine: return "quarantine";
    case bad_row_policy_t::fail:       return "fail";
    case bad_row_policy_t::skip:
    default:                           return "skip";
    }
}

merope::c_record_reader::c_record_reader(const std::string& path, const csv_dialect_t& dialect,
                                 std::uint64_t begin, std::uint64_t end, bool skip_header)
    : m_dialect(dialect), m_end(end) {
    std::error_code ec;
    m_file_size = std::filesystem::file_size(path, ec);
    if (ec) m_file_size = 0;
    if (m_end == k_whole_file || m_end > m_file_size) m_end = m_file_size;

    m_stream.open(path, std::ios::binary);
    if (!m_stream) throw std::runtime_error("cannot open dataset: " + path);

    m_transcode = m_dialect.encoding == encoding_t::windows_1250 ||
                  m_dialect.encoding == encoding_t::iso_8859_2;

    std::uint64_t start = begin;
    if (start == 0) {
        start = bom_length(m_dialect.encoding);
    }

    bool already_aligned = begin == 0;
    if (begin > 0) {
        m_stream.seekg(static_cast<std::streamoff>(begin - 1), std::ios::beg);
        char previous = '\0';
        m_stream.read(&previous, 1);
        already_aligned = m_stream.gcount() == 1 && previous == '\n';
        m_stream.clear();
    }

    m_stream.seekg(static_cast<std::streamoff>(start), std::ios::beg);
    m_buffer_origin = start;

    if (begin > 0 && !already_aligned) {
        // Land on a record boundary: everything up to and including the next
        // newline belongs to the previous partition.
        refill();
        while (true) {
            const std::size_t newline = m_buffer.find('\n', m_cursor);
            if (newline != std::string::npos) {
                m_cursor = newline + 1;
                break;
            }
            m_cursor = m_buffer.size();
            if (!refill()) break;
        }
    } else if (skip_header && m_dialect.has_header) {
        std::vector<std::string> header;
        next(header);
        m_stats.rows_read = 0;  // the header is not data
    }
}

bool merope::c_record_reader::refill() {
    if (m_exhausted) return false;

    // Drop what has already been consumed, keep the tail of a partial record.
    if (m_cursor > 0) {
        m_buffer.erase(0, m_cursor);
        m_buffer_origin += m_cursor;
        m_cursor = 0;
    }

    const std::size_t before = m_buffer.size();
    m_buffer.resize(before + k_read_buffer);
    m_stream.read(m_buffer.data() + before, static_cast<std::streamsize>(k_read_buffer));
    const std::size_t got = static_cast<std::size_t>(m_stream.gcount());
    m_buffer.resize(before + got);

    if (got == 0) {
        m_exhausted = true;
        return false;
    }
    return true;
}

bool merope::c_record_reader::next(std::vector<std::string>& fields) {
    while (true) {
        // A partition owns every record that starts inside its range, and
        // reads past the end only far enough to finish that last record.
        if (m_buffer_origin + m_cursor >= m_end) return false;

        const std::size_t   before = m_cursor;
        const record_status_t status = parse_record(m_buffer, m_cursor, m_dialect, fields);

        if (status == record_status_t::ok) {
            m_stats.rows_read++;
            m_stats.bytes_consumed += m_cursor - before;
            if (m_transcode) {
                for (std::string& field : fields) {
                    field = transcode_to_utf8(field, m_dialect.encoding);
                }
            }
            return true;
        }

        if (refill()) continue;

        // Stream is done. A trailing record with no newline is still a record.
        if (m_cursor < m_buffer.size()) {
            m_buffer.push_back('\n');
            const std::size_t retry_before = m_cursor;
            if (parse_record(m_buffer, m_cursor, m_dialect, fields) == record_status_t::ok) {
                m_stats.rows_read++;
                m_stats.bytes_consumed += m_cursor - retry_before;
                if (m_transcode) {
                    for (std::string& field : fields) {
                        field = transcode_to_utf8(field, m_dialect.encoding);
                    }
                }
                m_buffer.clear();
                m_cursor = 0;
                return true;
            }
        }
        return false;
    }
}

merope::c_chunk_reader::c_chunk_reader(const schema_t& schema, std::vector<std::size_t> projection,
                               std::uint64_t begin, std::uint64_t end,
                               bad_row_policy_t policy, std::size_t chunk_rows)
    : m_schema(schema),
      m_projection(std::move(projection)),
      m_options(schema.parse_options()),
      m_policy(policy),
      m_chunk_rows(chunk_rows == 0 ? k_default_chunk_rows : chunk_rows),
      m_reader(schema.dataset_path, schema.dialect, begin, end, begin == 0) {
    m_types.reserve(m_projection.size());
    for (const std::size_t physical : m_projection) {
        if (physical >= m_schema.columns.size()) {
            throw std::runtime_error("projection refers to a column outside the schema");
        }
        m_types.push_back(m_schema.columns[physical].physical_type);
    }
}

merope::read_stats_t merope::c_chunk_reader::stats() const noexcept {
    read_stats_t combined = m_reader.stats();
    combined.bad_rows    = m_bad_rows;
    combined.quarantined = m_quarantine.size();
    // rows_read counts what the raw reader produced; the bad ones never reached
    // a chunk, so report the rows the engine actually saw.
    combined.rows_read = combined.rows_read >= m_bad_rows ? combined.rows_read - m_bad_rows : 0;
    return combined;
}

bool merope::c_chunk_reader::next_chunk(chunk_t& chunk) {
    // The chunk is reused across calls, and may carry computed columns the
    // engine appended after the scanned ones. Only the scanned prefix is ours.
    if (chunk.columns.size() < m_projection.size()) {
        chunk.columns.resize(m_projection.size());
    }
    for (std::size_t k = 0; k < m_projection.size(); ++k) {
        column_block_t& column = chunk.columns[k];
        if (column.type != m_types[k]) {
            column.type = m_types[k];
            column.reserve(m_chunk_rows);
        }
        column.clear();
    }
    chunk.row_count = 0;

    // Scratch for one row, so a row that turns out to be bad never leaves a
    // half written value behind in the chunk.
    std::vector<std::uint8_t>  row_null(m_projection.size(), 0);
    std::vector<std::int64_t>  row_int(m_projection.size(), 0);
    std::vector<double>        row_real(m_projection.size(), 0.0);
    std::vector<std::string>   row_text(m_projection.size());

    const std::size_t expected_columns = m_schema.columns.size();

    while (chunk.row_count < m_chunk_rows) {
        if (!m_reader.next(m_fields)) break;

        bool row_ok = m_fields.size() == expected_columns;
        if (row_ok) {
            for (std::size_t k = 0; k < m_projection.size() && row_ok; ++k) {
                const std::string& field = m_fields[m_projection[k]];
                if (looks_like_null(field, is_string_like(m_types[k]))) {
                    row_null[k] = 1;
                    continue;
                }
                row_null[k] = 0;
                if (!parse_as(field, m_types[k], m_options, row_int[k], row_real[k], row_text[k])) {
                    row_ok = false;
                }
            }
        }

        if (!row_ok) {
            ++m_bad_rows;
            switch (m_policy) {
            case bad_row_policy_t::fail:
                throw std::runtime_error("row " + std::to_string(m_reader.stats().rows_read) +
                                         " does not match the schema: " +
                                         join_fields(m_fields, m_schema.dialect.delimiter));
            case bad_row_policy_t::quarantine:
                if (m_quarantine.size() < k_max_quarantined) {
                    m_quarantine.push_back(join_fields(m_fields, m_schema.dialect.delimiter));
                }
                break;
            case bad_row_policy_t::skip:
                break;
            }
            continue;
        }

        for (std::size_t k = 0; k < m_projection.size(); ++k) {
            column_block_t& column = chunk.columns[k];
            if (row_null[k] != 0) {
                column.push_null();
                continue;
            }
            switch (storage_kind_of(m_types[k])) {
            case storage_kind_t::integer: column.push_int(row_int[k]);            break;
            case storage_kind_t::real:    column.push_real(row_real[k]);          break;
            case storage_kind_t::text:    column.push_text(std::move(row_text[k])); break;
            case storage_kind_t::none:    column.push_null();                     break;
            }
        }
        ++chunk.row_count;
    }

    return chunk.row_count > 0;
}

