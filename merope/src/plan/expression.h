#pragma once

#include "../core/types.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace merope {

enum class expr_kind_t : std::uint8_t {
    column,
    literal,
    function,
    binary,
    unary_negate,
    logical_not,
    in_list,
    between,
    is_null
};

enum class binary_op_t : std::uint8_t {
    add, subtract, multiply, divide,
    equal, not_equal, less, less_equal, greater, greater_equal,
    logical_and, logical_or
};

enum class function_t : std::uint8_t {
    year, month, day, hour, minute,
    lower, upper, length, abs_value
};

const char* to_string(binary_op_t op) noexcept;
const char* to_string(function_t function) noexcept;
bool        function_from_string(std::string_view name, function_t& out) noexcept;
bool        is_comparison(binary_op_t op) noexcept;
bool        is_arithmetic(binary_op_t op) noexcept;
bool        is_logical(binary_op_t op) noexcept;

struct expr_t;
using expr_ptr = std::unique_ptr<expr_t>;

struct expr_t {
    expr_kind_t kind = expr_kind_t::literal;

    // kind == column
    std::string name;                              // as written in the plan
    std::size_t chunk_index = static_cast<std::size_t>(-1);  // filled in by the compiler

    // kind == literal
    cell_value_t literal;
    data_type_t  literal_type = data_type_t::unknown;

    // kind == function
    function_t function = function_t::year;

    // kind == binary
    binary_op_t op = binary_op_t::equal;

    // Operands. between has three, is_null and the unary forms have one.
    std::vector<expr_ptr> args;

    // Filled in by the compiler; unknown until then.
    data_type_t result_type = data_type_t::unknown;

    // True for is_null / logical forms that ignore null operands.
    bool negated = false;
};

expr_ptr make_column(std::string name);
expr_ptr make_literal(cell_value_t value, data_type_t type);
expr_ptr make_binary(binary_op_t op, expr_ptr left, expr_ptr right);

// Parses the tiny expression grammar. On failure returns nullptr and fills
// `error` with a message naming the offending token.
expr_ptr parse_expression(std::string_view text, std::string& error);

// Round trips an expression back to text, for the plan echo shown to the user.
std::string expression_to_string(const expr_t& expr);

// Collects every column name the expression reads.
void collect_columns(const expr_t& expr, std::vector<std::string>& out);

}
