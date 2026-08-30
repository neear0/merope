// plan/query_plan.h - the declarative plan the AI is allowed to produce
// (spec 5). It never contains code, only named operations over named columns.
#pragma once

#include "../core/json.h"
#include "expression.h"

#include <cstdint>
#include <string>
#include <vector>

namespace merope {

enum class operation_type_t : std::uint8_t {
    project,    // computed column: expr + as, or a plain column list
    filter,     // predicate over columns
    group_by,   // one or more grouping columns
    aggregate,  // COUNT, SUM, AVG, MIN, MAX
    sort,
    limit
};

enum class aggregate_function_t : std::uint8_t { count, sum, average, minimum, maximum };
enum class sort_order_t : std::uint8_t { ascending, descending };

const char* to_string(operation_type_t type) noexcept;
const char* to_string(aggregate_function_t function) noexcept;
const char* to_string(sort_order_t order) noexcept;

bool operation_type_from_string(const std::string& name, operation_type_t& out) noexcept;
bool aggregate_function_from_string(const std::string& name, aggregate_function_t& out) noexcept;

struct operation_t {
    operation_type_t type = operation_type_t::filter;

    // project: either a computed expression with an alias, or a column list.
    std::string              expression_text;
    std::string              alias;
    std::vector<std::string> columns;

    // filter
    std::string predicate_text;

    // aggregate
    aggregate_function_t function = aggregate_function_t::count;
    std::string          column;  // empty for COUNT(*)

    // sort
    sort_order_t order = sort_order_t::ascending;

    // limit
    std::int64_t limit_rows = 0;
};

struct query_plan_t {
    std::string              natural_language_query;
    std::string              provider;  // which AI produced this, or "mock"
    std::vector<operation_t> operations;
};

bool parse_query_plan(const json_value_t& root, query_plan_t& out, std::string& error);
bool parse_query_plan(const std::string& text, query_plan_t& out, std::string& error);

json_value_t plan_to_json(const query_plan_t& plan);
std::string  format_plan(const query_plan_t& plan);

}
