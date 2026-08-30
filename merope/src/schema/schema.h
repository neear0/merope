#pragma once

#include "../core/parse.h"
#include "../core/types.h"
#include "../dataset/csv_format.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace merope {

struct column_schema_t {
    std::string     physical_name;
    std::size_t     physical_index = 0;
    data_type_t     physical_type  = data_type_t::unknown;
    std::string     semantic_name;
    semantic_type_t semantic_type  = semantic_type_t::unknown;
    double          confidence     = 0.0;
    bool            user_confirmed = false;

    // The name a query should use. Falls back to the physical name while the
    // semantic layer is still UNKNOWN.
    const std::string& query_name() const noexcept {
        return semantic_name.empty() ? physical_name : semantic_name;
    }
};

inline constexpr std::size_t k_invalid_column = static_cast<std::size_t>(-1);

struct schema_t {
    std::string                  dataset_path;
    csv_dialect_t                dialect;
    std::vector<column_schema_t> columns;

    std::size_t index_of(std::string_view name) const noexcept;

    const column_schema_t* find(std::string_view name) const noexcept;

    parse_options_t parse_options() const noexcept;

    // Every column the engine would have to read for a given set of names.
    std::vector<std::size_t> resolve_all(const std::vector<std::string>& names,
                                         std::vector<std::string>& unresolved) const;

    bool all_confirmed() const noexcept;
};

// Where the confirmed schema is cached so that AI inference does not re-run on
// every query (spec 4.5).
std::string schema_sidecar_path(const std::string& dataset_path);

bool save_schema(const schema_t& schema, const std::string& path, std::string& error);
bool load_schema(const std::string& path, schema_t& out, std::string& error);

std::string format_schema_table(const schema_t& schema);

}
