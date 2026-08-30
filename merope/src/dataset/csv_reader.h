// dataset/csv_reader.h - streaming CSV access.
//
// Two layers:
//   c_record_reader - raw fields over a byte range, aligned to line boundaries
//   c_chunk_reader  - typed columnar chunks for the engine, projection aware
//
// Neither ever holds more than one buffer plus one chunk in memory, which is
// what lets the engine work on files larger than RAM (spec 6.2, 6.3).
#pragma once

#include "../engine/chunk.h"
#include "../schema/schema.h"
#include "csv_format.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace merope {

// What to do with a row that does not fit the schema (spec 6.3).
enum class bad_row_policy_t : std::uint8_t { skip, quarantine, fail };

bad_row_policy_t bad_row_policy_from_string(const std::string& name) noexcept;
const char*      to_string(bad_row_policy_t policy) noexcept;

struct read_stats_t {
    std::uint64_t rows_read       = 0;
    std::uint64_t bad_rows        = 0;
    std::uint64_t quarantined     = 0;
    std::uint64_t bytes_consumed  = 0;

    void merge(const read_stats_t& other) noexcept {
        rows_read      += other.rows_read;
        bad_rows       += other.bad_rows;
        quarantined    += other.quarantined;
        bytes_consumed += other.bytes_consumed;
    }
};

inline constexpr std::uint64_t k_whole_file  = static_cast<std::uint64_t>(-1);
inline constexpr std::size_t   k_read_buffer = 1 << 20;  // 1 MiB

// Reads records from [begin, end). If begin > 0 the reader skips forward to
// just past the next newline, so a partition never starts mid record; it then
// reads past `end` only far enough to finish the record already in progress.
class c_record_reader {
public:
    c_record_reader(const std::string& path, const csv_dialect_t& dialect,
                    std::uint64_t begin = 0, std::uint64_t end = k_whole_file,
                    bool skip_header = false);

    // Fills `fields` with the next record. Returns false at the end of the range.
    bool next(std::vector<std::string>& fields);

    const read_stats_t& stats() const noexcept { return m_stats; }
    std::uint64_t       file_size() const noexcept { return m_file_size; }

private:
    bool refill();

    std::ifstream m_stream;
    csv_dialect_t m_dialect;
    std::string   m_buffer;
    std::size_t   m_cursor        = 0;
    std::uint64_t m_buffer_origin = 0;   // absolute offset of m_buffer[0]
    std::uint64_t m_end           = k_whole_file;
    std::uint64_t m_file_size     = 0;
    bool          m_exhausted     = false;
    bool          m_transcode     = false;
    read_stats_t  m_stats;
};

// Produces typed chunks for exactly the columns the plan asked for.
class c_chunk_reader {
public:
    c_chunk_reader(const schema_t& schema, std::vector<std::size_t> projection,
                   std::uint64_t begin = 0, std::uint64_t end = k_whole_file,
                   bad_row_policy_t policy = bad_row_policy_t::skip,
                   std::size_t chunk_rows = k_default_chunk_rows);

    // Fills `chunk` with up to chunk_rows rows. Returns false when the range is
    // exhausted. `chunk` is reused between calls; its buffers are not freed.
    bool next_chunk(chunk_t& chunk);

    // Combines the raw reader counters with the rows this layer rejected.
    read_stats_t                    stats() const noexcept;
    std::uint64_t                   bad_rows() const noexcept { return m_bad_rows; }
    const std::vector<std::string>& quarantine() const noexcept { return m_quarantine; }
    const std::vector<std::size_t>& projection() const noexcept { return m_projection; }

private:
    const schema_t&          m_schema;
    std::vector<std::size_t> m_projection;
    std::vector<data_type_t> m_types;
    parse_options_t          m_options;
    bad_row_policy_t         m_policy;
    std::size_t              m_chunk_rows;
    c_record_reader          m_reader;
    std::vector<std::string> m_fields;
    std::vector<std::string> m_quarantine;
    std::uint64_t            m_bad_rows = 0;
};

} // namespace merope
