// plan/plan_validator.h - the gate between the AI and the engine (spec 3.1, 5).
//
// The AI produces a logical plan as JSON. Nothing from it reaches the engine
// until this stage has resolved every column against the confirmed schema,
// checked that every operation makes sense for the types involved, and
// canonicalised the result into a physical plan the engine can execute.
#pragma once

#include "../schema/schema.h"
#include "expression.h"
#include "query_plan.h"

#include <cstdint>
#include <string>
#include <vector>

namespace merope {

// Applied when the plan would otherwise stream an unbounded result set.
inline constexpr std::int64_t k_default_result_limit = 1000;

// A slot is a value available to filters, grouping and aggregation:
// first the scanned columns in projection order, then computed columns.
using slot_index_t = std::size_t;

struct computed_column_t {
    std::string  name;
    expr_ptr     expression;
    data_type_t  type = data_type_t::unknown;
};

struct aggregate_spec_t {
    aggregate_function_t function = aggregate_function_t::count;
    std::string          alias;
    slot_index_t         input_slot = static_cast<slot_index_t>(-1);  // unset for COUNT(*)
    data_type_t          input_type  = data_type_t::unknown;
    data_type_t          output_type = data_type_t::int64;

    bool counts_rows() const noexcept {
        return function == aggregate_function_t::count && input_slot == static_cast<slot_index_t>(-1);
    }
    // AVG is never reduced as a mean of means; it carries (sum, count).
    bool needs_partial_sum_and_count() const noexcept {
        return function == aggregate_function_t::average;
    }
};

struct sort_spec_t {
    std::size_t  output_index = 0;   // index into the result columns
    sort_order_t order        = sort_order_t::ascending;
};

struct physical_plan_t {
    // Physical column indices the scan must read. Nothing else is parsed.
    std::vector<std::size_t> projection;

    std::vector<computed_column_t> computed;
    expr_ptr                       filter;         // may be null

    std::vector<slot_index_t> group_by;
    std::vector<std::string>  group_by_names;

    std::vector<aggregate_spec_t> aggregates;

    // When there is no aggregation the plan emits rows made of these slots.
    std::vector<slot_index_t> output_slots;
    std::vector<std::string>  output_names;

    std::vector<sort_spec_t> sorts;
    std::int64_t             limit = 0;   // 0 means no limit

    // Type of every slot: scanned columns first, then computed ones. The
    // engine needs this to build chunks and to format the result.
    std::vector<data_type_t> slot_types;

    bool aggregating() const noexcept { return !aggregates.empty(); }

    // Column names of the result, in order.
    std::vector<std::string> result_columns() const;
    std::vector<data_type_t> result_types() const;
};

struct validation_result_t {
    bool                     accepted = false;
    std::vector<std::string> errors;    // why the plan was rejected
    std::vector<std::string> warnings;  // what the canonicaliser changed
    physical_plan_t          plan;
};

// Validates and canonicalises. On rejection `plan` is left empty and `errors`
// says exactly what was wrong, so the AI can be asked again with the reason.
validation_result_t validate_plan(const query_plan_t& logical, const schema_t& schema);

std::string format_physical_plan(const physical_plan_t& plan, const schema_t& schema);

} // namespace merope
