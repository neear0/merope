#include "evaluator.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

// Reads an integer-stored value (int64, bool, date, datetime, decimal).
inline std::int64_t int_at(const merope::value_block_t& value, std::size_t row) noexcept {
    return value.block->ints[value.index_for(row)];
}

inline double real_at(const merope::value_block_t& value, std::size_t row) noexcept {
    return value.block->reals[value.index_for(row)];
}

inline const std::string& text_at(const merope::value_block_t& value, std::size_t row) noexcept {
    return value.block->texts[value.index_for(row)];
}

inline bool null_at(const merope::value_block_t& value, std::size_t row) noexcept {
    return value.block->nulls[value.index_for(row)] != 0;
}

// Numeric value promoted to double, for mixed type arithmetic and comparison.
inline double numeric_at(const merope::value_block_t& value, std::size_t row) noexcept {
    switch (merope::storage_kind_of(value.type())) {
    case merope::storage_kind_t::real:    return real_at(value, row);
    case merope::storage_kind_t::integer:
        if (value.type() == merope::data_type_t::decimal) {
            return static_cast<double>(int_at(value, row)) / static_cast<double>(merope::k_money_factor);
        }
        return static_cast<double>(int_at(value, row));
    default:
        return 0.0;
    }
}

static int compare_values(const merope::value_block_t& left, const merope::value_block_t& right, std::size_t row) noexcept {
    const merope::storage_kind_t kind = merope::storage_kind_of(left.type());
    if (kind == merope::storage_kind_t::text) {
        const std::string& a = text_at(left, row);
        const std::string& b = text_at(right, row);
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    if (kind == merope::storage_kind_t::integer && merope::storage_kind_of(right.type()) == merope::storage_kind_t::integer) {
        const std::int64_t a = int_at(left, row);
        const std::int64_t b = int_at(right, row);
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    const double a = numeric_at(left, row);
    const double b = numeric_at(right, row);
    return a < b ? -1 : (a > b ? 1 : 0);
}

static bool comparison_holds(merope::binary_op_t op, int ordering) noexcept {
    switch (op) {
    case merope::binary_op_t::equal:         return ordering == 0;
    case merope::binary_op_t::not_equal:     return ordering != 0;
    case merope::binary_op_t::less:          return ordering < 0;
    case merope::binary_op_t::less_equal:    return ordering <= 0;
    case merope::binary_op_t::greater:       return ordering > 0;
    case merope::binary_op_t::greater_equal: return ordering >= 0;
    default:                         return false;
    }
}

static void days_and_seconds(const merope::value_block_t& value, std::size_t row,
                             std::int64_t& days, std::int64_t& seconds_of_day) noexcept {
    const std::int64_t raw = int_at(value, row);
    if (value.type() == merope::data_type_t::date) {
        days           = raw;
        seconds_of_day = 0;
        return;
    }
    days           = raw >= 0 ? raw / 86400 : (raw - 86399) / 86400;
    seconds_of_day = raw - days * 86400;
}

merope::column_block_t& merope::c_expression_evaluator::acquire(data_type_t type, std::size_t rows) {
    if (m_used == m_pool.size()) m_pool.push_back(std::make_unique<column_block_t>());
    column_block_t& block = *m_pool[m_used++];
    block.clear();
    block.type = type;
    block.reserve(rows);
    return block;
}

merope::value_block_t merope::c_expression_evaluator::evaluate(const expr_t& expr, const chunk_t& chunk) {
    m_used = 0;
    return eval_node(expr, chunk);
}

merope::column_block_t merope::c_expression_evaluator::evaluate_owned(const expr_t& expr, const chunk_t& chunk) {
    const value_block_t value = evaluate(expr, chunk);
    if (!value.is_scalar) return *value.block;

    // A constant expression still has to become a full column.
    column_block_t out;
    out.type = value.block->type;
    out.reserve(chunk.row_count);
    for (std::size_t row = 0; row < chunk.row_count; ++row) {
        if (value.block->nulls[0] != 0) {
            out.push_null();
            continue;
        }
        switch (storage_kind_of(out.type)) {
        case storage_kind_t::integer: out.push_int(value.block->ints[0]);   break;
        case storage_kind_t::real:    out.push_real(value.block->reals[0]); break;
        case storage_kind_t::text:    out.push_text(value.block->texts[0]); break;
        case storage_kind_t::none:    out.push_null();                      break;
        }
    }
    return out;
}

merope::value_block_t merope::c_expression_evaluator::eval_node(const expr_t& expr, const chunk_t& chunk) {
    const std::size_t rows = chunk.row_count;

    switch (expr.kind) {
    case expr_kind_t::column: {
        if (expr.chunk_index >= chunk.columns.size()) {
            throw std::runtime_error("expression refers to a slot the chunk does not have");
        }
        return value_block_t{&chunk.columns[expr.chunk_index], false};
    }

    case expr_kind_t::literal: {
        column_block_t& out = acquire(expr.literal_type, 1);
        if (is_null(expr.literal)) {
            out.push_null();
        } else if (const auto* integral = std::get_if<std::int64_t>(&expr.literal)) {
            out.push_int(*integral);
        } else if (const auto* flag = std::get_if<bool>(&expr.literal)) {
            out.push_int(*flag ? 1 : 0);
        } else if (const auto* real = std::get_if<double>(&expr.literal)) {
            out.push_real(*real);
        } else if (const auto* text = std::get_if<std::string>(&expr.literal)) {
            out.push_text(*text);
        }
        // A boolean literal is stored as an integer like every other bool.
        if (expr.literal_type == data_type_t::boolean) out.type = data_type_t::boolean;
        return value_block_t{&out, true};
    }

    case expr_kind_t::function: {
        const value_block_t argument = eval_node(*expr.args[0], chunk);
        column_block_t&     out      = acquire(expr.result_type, rows);
        for (std::size_t row = 0; row < rows; ++row) {
            if (null_at(argument, row)) { out.push_null(); continue; }
            switch (expr.function) {
            case function_t::year:
            case function_t::month:
            case function_t::day: {
                std::int64_t days = 0, seconds = 0;
                days_and_seconds(argument, row, days, seconds);
                int year = 0; unsigned month = 0, day = 0;
                civil_from_days(days, year, month, day);
                out.push_int(expr.function == function_t::year    ? year
                             : expr.function == function_t::month ? static_cast<std::int64_t>(month)
                                                                  : static_cast<std::int64_t>(day));
                break;
            }
            case function_t::hour:
            case function_t::minute: {
                std::int64_t days = 0, seconds = 0;
                days_and_seconds(argument, row, days, seconds);
                out.push_int(expr.function == function_t::hour ? seconds / 3600 : (seconds / 60) % 60);
                break;
            }
            case function_t::lower:
            case function_t::upper: {
                std::string text = text_at(argument, row);
                // ASCII only on purpose: case folding UTF-8 correctly needs a
                // Unicode table, and a half correct fold is worse than none.
                std::transform(text.begin(), text.end(), text.begin(), [&](unsigned char c) {
                    return static_cast<char>(expr.function == function_t::lower ? std::tolower(c)
                                                                                : std::toupper(c));
                });
                out.push_text(std::move(text));
                break;
            }
            case function_t::length:
                out.push_int(static_cast<std::int64_t>(text_at(argument, row).size()));
                break;
            case function_t::abs_value:
                if (storage_kind_of(argument.type()) == storage_kind_t::real) {
                    out.push_real(std::fabs(real_at(argument, row)));
                } else {
                    const std::int64_t value = int_at(argument, row);
                    out.push_int(value < 0 ? -value : value);
                }
                break;
            default:
                out.push_null();
                break;
            }
        }
        return value_block_t{&out, false};
    }

    case expr_kind_t::unary_negate: {
        const value_block_t argument = eval_node(*expr.args[0], chunk);
        column_block_t&     out      = acquire(expr.result_type, rows);
        for (std::size_t row = 0; row < rows; ++row) {
            if (null_at(argument, row)) { out.push_null(); continue; }
            if (storage_kind_of(expr.result_type) == storage_kind_t::real) {
                out.push_real(-numeric_at(argument, row));
            } else {
                out.push_int(-int_at(argument, row));
            }
        }
        return value_block_t{&out, false};
    }

    case expr_kind_t::logical_not: {
        const value_block_t argument = eval_node(*expr.args[0], chunk);
        column_block_t&     out      = acquire(data_type_t::boolean, rows);
        for (std::size_t row = 0; row < rows; ++row) {
            if (null_at(argument, row)) { out.push_null(); continue; }
            out.push_int(int_at(argument, row) != 0 ? 0 : 1);
        }
        return value_block_t{&out, false};
    }

    case expr_kind_t::is_null: {
        const value_block_t argument = eval_node(*expr.args[0], chunk);
        column_block_t&     out      = acquire(data_type_t::boolean, rows);
        for (std::size_t row = 0; row < rows; ++row) {
            const bool missing = null_at(argument, row);
            out.push_int((missing != expr.negated) ? 1 : 0);
        }
        return value_block_t{&out, false};
    }

    case expr_kind_t::in_list: {
        const value_block_t subject = eval_node(*expr.args[0], chunk);
        std::vector<value_block_t> items;
        items.reserve(expr.args.size() - 1);
        for (std::size_t index = 1; index < expr.args.size(); ++index) {
            items.push_back(eval_node(*expr.args[index], chunk));
        }
        column_block_t& out = acquire(data_type_t::boolean, rows);
        for (std::size_t row = 0; row < rows; ++row) {
            if (null_at(subject, row)) { out.push_null(); continue; }
            bool found = false;
            for (const value_block_t& item : items) {
                if (null_at(item, row)) continue;
                if (compare_values(subject, item, row) == 0) { found = true; break; }
            }
            out.push_int((found != expr.negated) ? 1 : 0);
        }
        return value_block_t{&out, false};
    }

    case expr_kind_t::between: {
        const value_block_t subject = eval_node(*expr.args[0], chunk);
        const value_block_t low     = eval_node(*expr.args[1], chunk);
        const value_block_t high    = eval_node(*expr.args[2], chunk);
        column_block_t&     out     = acquire(data_type_t::boolean, rows);
        for (std::size_t row = 0; row < rows; ++row) {
            if (null_at(subject, row) || null_at(low, row) || null_at(high, row)) {
                out.push_null();
                continue;
            }
            const bool inside = compare_values(subject, low, row) >= 0 &&
                                compare_values(subject, high, row) <= 0;
            out.push_int((inside != expr.negated) ? 1 : 0);
        }
        return value_block_t{&out, false};
    }

    case expr_kind_t::binary: {
        const value_block_t left  = eval_node(*expr.args[0], chunk);
        const value_block_t right = eval_node(*expr.args[1], chunk);

        if (is_logical(expr.op)) {
            column_block_t& out = acquire(data_type_t::boolean, rows);
            for (std::size_t row = 0; row < rows; ++row) {
                const bool left_null  = null_at(left, row);
                const bool right_null = null_at(right, row);
                const bool left_true  = !left_null && int_at(left, row) != 0;
                const bool right_true = !right_null && int_at(right, row) != 0;

                // Three valued logic: FALSE AND NULL is FALSE, TRUE OR NULL is TRUE.
                if (expr.op == binary_op_t::logical_and) {
                    if ((!left_null && !left_true) || (!right_null && !right_true)) {
                        out.push_int(0);
                    } else if (left_null || right_null) {
                        out.push_null();
                    } else {
                        out.push_int(1);
                    }
                } else {
                    if (left_true || right_true) {
                        out.push_int(1);
                    } else if (left_null || right_null) {
                        out.push_null();
                    } else {
                        out.push_int(0);
                    }
                }
            }
            return value_block_t{&out, false};
        }

        if (is_comparison(expr.op)) {
            column_block_t& out = acquire(data_type_t::boolean, rows);
            for (std::size_t row = 0; row < rows; ++row) {
                if (null_at(left, row) || null_at(right, row)) { out.push_null(); continue; }
                out.push_int(comparison_holds(expr.op, compare_values(left, right, row)) ? 1 : 0);
            }
            return value_block_t{&out, false};
        }

        column_block_t& out = acquire(expr.result_type, rows);
        const bool      integral_result = storage_kind_of(expr.result_type) == storage_kind_t::integer;

        for (std::size_t row = 0; row < rows; ++row) {
            if (null_at(left, row) || null_at(right, row)) { out.push_null(); continue; }

            if (expr.op == binary_op_t::divide) {
                const double divisor = numeric_at(right, row);
                // Division by zero produces NULL rather than an infinity that
                // would then poison every downstream aggregate.
                if (divisor == 0.0) { out.push_null(); continue; }
                out.push_real(numeric_at(left, row) / divisor);
                continue;
            }

            if (!integral_result) {
                const double a = numeric_at(left, row);
                const double b = numeric_at(right, row);
                switch (expr.op) {
                case binary_op_t::add:      out.push_real(a + b); break;
                case binary_op_t::subtract: out.push_real(a - b); break;
                case binary_op_t::multiply: out.push_real(a * b); break;
                default:                    out.push_null();      break;
                }
                continue;
            }

            const std::int64_t a = int_at(left, row);
            const std::int64_t b = int_at(right, row);
            switch (expr.op) {
            case binary_op_t::add:      out.push_int(a + b); break;
            case binary_op_t::subtract: out.push_int(a - b); break;
            case binary_op_t::multiply:
                // Scaled money times a plain integer keeps the scale; the
                // validator has already refused decimal times decimal.
                out.push_int(a * b);
                break;
            default:
                out.push_null();
                break;
            }
        }
        return value_block_t{&out, false};
    }

    default:
        throw std::runtime_error("cannot evaluate this expression");
    }
}

