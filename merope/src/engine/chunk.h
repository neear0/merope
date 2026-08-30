#pragma once

#include "../core/types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace merope {

inline constexpr std::size_t k_default_chunk_rows = 32768;

// One column of a chunk. Only the vector matching the storage kind of `type`
// is populated; the others stay empty.
struct column_block_t {
    data_type_t               type = data_type_t::unknown;
    std::vector<std::uint8_t> nulls;   // 1 = the value is missing
    std::vector<std::int64_t> ints;    // int64, bool, date, datetime, decimal
    std::vector<double>       reals;   // float64
    std::vector<std::string>  texts;   // utf8, categorical

    std::size_t size() const noexcept { return nulls.size(); }

    void clear() noexcept {
        nulls.clear();
        ints.clear();
        reals.clear();
        texts.clear();
    }

    void reserve(std::size_t rows) {
        nulls.reserve(rows);
        switch (storage_kind_of(type)) {
        case storage_kind_t::integer: ints.reserve(rows);  break;
        case storage_kind_t::real:    reals.reserve(rows); break;
        case storage_kind_t::text:    texts.reserve(rows); break;
        case storage_kind_t::none:                         break;
        }
    }

    // Every push keeps the null flag vector and the value vector the same
    // length, so `nulls.size()` is always the row count.
    void push_null() {
        nulls.push_back(1);
        switch (storage_kind_of(type)) {
        case storage_kind_t::integer: ints.push_back(0);        break;
        case storage_kind_t::real:    reals.push_back(0.0);     break;
        case storage_kind_t::text:    texts.emplace_back();     break;
        case storage_kind_t::none:                              break;
        }
    }

    void push_int(std::int64_t value) {
        nulls.push_back(0);
        ints.push_back(value);
    }

    void push_real(double value) {
        nulls.push_back(0);
        reals.push_back(value);
    }

    void push_text(std::string value) {
        nulls.push_back(0);
        texts.push_back(std::move(value));
    }

    bool is_null(std::size_t row) const noexcept { return nulls[row] != 0; }

    cell_value_t value_at(std::size_t row) const {
        if (is_null(row)) return cell_value_t{};
        switch (storage_kind_of(type)) {
        case storage_kind_t::integer: return cell_value_t{ints[row]};
        case storage_kind_t::real:    return cell_value_t{reals[row]};
        case storage_kind_t::text:    return cell_value_t{texts[row]};
        case storage_kind_t::none:
        default:                      return cell_value_t{};
        }
    }
};

// A batch of rows, one block per projected column. Chunk column k maps to the
// physical column `projection[k]` held by the reader that produced it.
struct chunk_t {
    std::vector<column_block_t> columns;
    std::size_t                 row_count = 0;

    void clear() noexcept {
        for (column_block_t& column : columns) column.clear();
        row_count = 0;
    }

    bool empty() const noexcept { return row_count == 0; }
};

}
