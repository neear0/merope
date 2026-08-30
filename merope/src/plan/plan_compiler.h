#pragma once

#include "../core/parse.h"
#include "../schema/schema.h"
#include "expression.h"
#include "plan_validator.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace merope {

inline constexpr slot_index_t k_no_slot = static_cast<slot_index_t>(-1);

inline constexpr slot_index_t k_computed_flag = static_cast<slot_index_t>(1) << 40;

bool is_computed_slot(slot_index_t slot) noexcept;

// Tracks what a name refers to while the plan is being compiled: a scanned
// column, or a computed column produced by an earlier project operation.
class c_slot_table {
public:
    explicit c_slot_table(const schema_t& schema);

    // Binds a schema column, adding it to the projection the first time it is
    // touched. Returns k_no_slot when the name is not in the schema.
    slot_index_t bind_column(const std::string& name);
    slot_index_t add_computed(const std::string& name, data_type_t type);
    slot_index_t lookup(const std::string& name) const;
    data_type_t  type_of(slot_index_t slot) const;

    // Compile time slot number to the position it will occupy in a chunk.
    slot_index_t final_slot(slot_index_t slot) const noexcept;

    const std::vector<std::size_t>& projection() const noexcept { return m_projection; }
    std::size_t                     scanned_count() const noexcept { return m_projection.size(); }

    std::vector<data_type_t> all_types() const;

private:
    const schema_t&                               m_schema;
    std::vector<std::size_t>                      m_projection;
    std::vector<data_type_t>                      m_scanned_types;
    std::vector<data_type_t>                      m_computed_types;
    std::unordered_map<std::string, slot_index_t> m_by_name;
};

// Resolves names, checks types and annotates result_type on an expression.
class c_expr_compiler {
public:
    c_expr_compiler(c_slot_table& slots, const parse_options_t& options,
                    std::vector<std::string>& errors);

    // Returns data_type_t::unknown when the expression is not acceptable.
    data_type_t compile(expr_t& node);

private:
    data_type_t compile_function(expr_t& node);
    data_type_t compile_in_list(expr_t& node);
    data_type_t compile_between(expr_t& node);
    data_type_t compile_binary(expr_t& node);

    c_slot_table&             m_slots;
    parse_options_t           m_options;
    std::vector<std::string>& m_errors;
};

}
