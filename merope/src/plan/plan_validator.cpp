#include "plan_validator.h"

#include "../core/parse.h"
#include "plan_compiler.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>

static std::string lower_copy(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Rewrites every column reference in an expression to its final chunk position.
static void remap_slots(merope::expr_t& node, const merope::c_slot_table& slots) {
    if (node.kind == merope::expr_kind_t::column) {
        node.chunk_index = slots.final_slot(node.chunk_index);
        return;
    }
    for (const merope::expr_ptr& argument : node.args) {
        if (argument != nullptr) remap_slots(*argument, slots);
    }
}

// Rewrites a literal so it can be compared against `target` without a lossy
// conversion at run time. Returns false when the comparison is nonsense.
static bool coerce_literal(merope::expr_t& node, merope::data_type_t target, const merope::parse_options_t& options,
                           std::string& error) {
    if (node.kind != merope::expr_kind_t::literal) return true;
    if (merope::is_null(node.literal)) return true;
    if (node.literal_type == target) return true;

    switch (target) {
    case merope::data_type_t::decimal: {
        // Money literals become scaled integers so the comparison is exact.
        if (const auto* real = std::get_if<double>(&node.literal)) {
            node.literal = static_cast<std::int64_t>(*real * static_cast<double>(merope::k_money_factor) +
                                                     (*real < 0 ? -0.5 : 0.5));
        } else if (const auto* integral = std::get_if<std::int64_t>(&node.literal)) {
            node.literal = *integral * merope::k_money_factor;
        } else if (const auto* text = std::get_if<std::string>(&node.literal)) {
            std::int64_t scaled = 0;
            if (!merope::parse_decimal(*text, options, scaled)) {
                error = "'" + *text + "' is not a monetary value";
                return false;
            }
            node.literal = scaled;
        } else {
            error = "cannot compare this literal with a DECIMAL column";
            return false;
        }
        break;
    }
    case merope::data_type_t::date: {
        const auto* text = std::get_if<std::string>(&node.literal);
        if (text == nullptr) {
            error = "a DATE column must be compared with a date literal such as '2026-01-31'";
            return false;
        }
        std::int64_t   days    = 0;
        merope::date_pattern_t pattern = merope::date_pattern_t::none;
        if (!merope::parse_date(*text, days, pattern)) {
            error = "'" + *text + "' is not a date";
            return false;
        }
        node.literal = days;
        break;
    }
    case merope::data_type_t::datetime: {
        const auto* text = std::get_if<std::string>(&node.literal);
        if (text == nullptr) {
            error = "a DATETIME column must be compared with a timestamp literal";
            return false;
        }
        std::int64_t   seconds = 0;
        merope::date_pattern_t pattern = merope::date_pattern_t::none;
        if (!merope::parse_datetime(*text, seconds, pattern)) {
            error = "'" + *text + "' is not a timestamp";
            return false;
        }
        node.literal = seconds;
        break;
    }
    case merope::data_type_t::int64: {
        if (const auto* real = std::get_if<double>(&node.literal)) {
            // Keep the comparison honest: 2025.5 is not an integer, so the
            // whole comparison is promoted to floating point instead.
            if (*real != static_cast<double>(static_cast<std::int64_t>(*real))) return true;
            node.literal = static_cast<std::int64_t>(*real);
        } else if (std::holds_alternative<std::string>(node.literal)) {
            error = "cannot compare an INT64 column with a text literal";
            return false;
        }
        break;
    }
    case merope::data_type_t::float64: {
        if (const auto* integral = std::get_if<std::int64_t>(&node.literal)) {
            node.literal = static_cast<double>(*integral);
        } else if (std::holds_alternative<std::string>(node.literal)) {
            error = "cannot compare a FLOAT64 column with a text literal";
            return false;
        }
        break;
    }
    case merope::data_type_t::boolean: {
        if (const auto* integral = std::get_if<std::int64_t>(&node.literal)) {
            if (*integral != 0 && *integral != 1) {
                error = "a BOOL column can only be compared with true, false, 0 or 1";
                return false;
            }
            node.literal = *integral != 0;
        } else if (const auto* text = std::get_if<std::string>(&node.literal)) {
            bool flag = false;
            if (!merope::parse_bool(*text, flag)) {
                error = "'" + *text + "' is not a boolean";
                return false;
            }
            node.literal = flag;
        }
        break;
    }
    case merope::data_type_t::utf8:
    case merope::data_type_t::categorical: {
        if (!std::holds_alternative<std::string>(node.literal)) {
            error = "a text column must be compared with a quoted literal";
            return false;
        }
        break;
    }
    default:
        break;
    }

    node.literal_type = target;
    node.result_type  = target;
    return true;
}

static merope::data_type_t unify_numeric(merope::data_type_t left, merope::data_type_t right) noexcept {
    if (left == merope::data_type_t::float64 || right == merope::data_type_t::float64) return merope::data_type_t::float64;
    if (left == merope::data_type_t::decimal || right == merope::data_type_t::decimal) return merope::data_type_t::decimal;
    return merope::data_type_t::int64;
}

merope::c_expr_compiler::c_expr_compiler(c_slot_table& slots, const parse_options_t& options,
                                          std::vector<std::string>& errors)
    : m_slots(slots), m_options(options), m_errors(errors) {}

// Resolves names, checks types and annotates result_type. Returns
// data_type_t::unknown when the expression is not acceptable.
merope::data_type_t merope::c_expr_compiler::compile(expr_t& node) {
    switch (node.kind) {
    case expr_kind_t::column: {
        const slot_index_t slot = m_slots.bind_column(node.name);
        if (slot == k_no_slot) {
            m_errors.push_back("unknown column '" + node.name + "'");
            return data_type_t::unknown;
        }
        node.chunk_index = slot;
        node.result_type = m_slots.type_of(slot);
        return node.result_type;
    }
    case expr_kind_t::literal:
        return node.result_type = node.literal_type;

    case expr_kind_t::function:
        return compile_function(node);

    case expr_kind_t::unary_negate: {
        const data_type_t operand = compile(*node.args[0]);
        if (operand == data_type_t::unknown) return operand;
        if (!is_numeric(operand)) {
            m_errors.push_back(std::string("cannot negate a ") + to_string(operand) + " value");
            return data_type_t::unknown;
        }
        return node.result_type = operand;
    }

    case expr_kind_t::logical_not: {
        const data_type_t operand = compile(*node.args[0]);
        if (operand == data_type_t::unknown) return operand;
        if (operand != data_type_t::boolean) {
            m_errors.push_back("NOT expects a boolean expression");
            return data_type_t::unknown;
        }
        return node.result_type = data_type_t::boolean;
    }

    case expr_kind_t::is_null:
        if (compile(*node.args[0]) == data_type_t::unknown) return data_type_t::unknown;
        return node.result_type = data_type_t::boolean;

    case expr_kind_t::in_list:
        return compile_in_list(node);

    case expr_kind_t::between:
        return compile_between(node);

    case expr_kind_t::binary:
        return compile_binary(node);

    default:
        m_errors.push_back("unsupported expression");
        return data_type_t::unknown;
    }
}

merope::data_type_t merope::c_expr_compiler::compile_function(expr_t& node) {
    const data_type_t argument = compile(*node.args[0]);
    if (argument == data_type_t::unknown) return argument;

    switch (node.function) {
    case function_t::year:
    case function_t::month:
    case function_t::day:
        if (!is_temporal(argument)) {
            m_errors.push_back(std::string(to_string(node.function)) +
                               "() needs a DATE or DATETIME column, got " + to_string(argument));
            return data_type_t::unknown;
        }
        return node.result_type = data_type_t::int64;
    case function_t::hour:
    case function_t::minute:
        if (argument != data_type_t::datetime) {
            m_errors.push_back(std::string(to_string(node.function)) +
                               "() needs a DATETIME column, got " + to_string(argument));
            return data_type_t::unknown;
        }
        return node.result_type = data_type_t::int64;
    case function_t::lower:
    case function_t::upper:
        if (!is_string_like(argument)) {
            m_errors.push_back(std::string(to_string(node.function)) + "() needs a text column");
            return data_type_t::unknown;
        }
        return node.result_type = argument;
    case function_t::length:
        if (!is_string_like(argument)) {
            m_errors.push_back("length() needs a text column");
            return data_type_t::unknown;
        }
        return node.result_type = data_type_t::int64;
    case function_t::abs_value:
        if (!is_numeric(argument)) {
            m_errors.push_back("abs() needs a numeric column");
            return data_type_t::unknown;
        }
        return node.result_type = argument;
    default:
        m_errors.push_back("unknown function");
        return data_type_t::unknown;
    }
}

merope::data_type_t merope::c_expr_compiler::compile_in_list(expr_t& node) {
    const data_type_t subject = compile(*node.args[0]);
    if (subject == data_type_t::unknown) return subject;

    for (std::size_t index = 1; index < node.args.size(); ++index) {
        expr_t& item = *node.args[index];
        if (compile(item) == data_type_t::unknown) return data_type_t::unknown;
        std::string reason;
        if (!coerce_literal(item, subject, m_options, reason)) {
            m_errors.push_back("IN list: " + reason);
            return data_type_t::unknown;
        }
        if (item.kind != expr_kind_t::literal && item.result_type != subject) {
            m_errors.push_back("IN list mixes incompatible types");
            return data_type_t::unknown;
        }
    }
    return node.result_type = data_type_t::boolean;
}

merope::data_type_t merope::c_expr_compiler::compile_between(expr_t& node) {
    const data_type_t subject = compile(*node.args[0]);
    if (subject == data_type_t::unknown) return subject;
    if (is_string_like(subject) && subject == data_type_t::utf8) {
        // Ranges over free text are almost always a mistake by the planner.
        m_errors.push_back("BETWEEN is not allowed on free text columns");
        return data_type_t::unknown;
    }
    for (std::size_t index = 1; index <= 2; ++index) {
        expr_t& bound = *node.args[index];
        if (compile(bound) == data_type_t::unknown) return data_type_t::unknown;
        // coerce_literal() waves through anything that is not a literal, so a
        // column or expression bound needs the same check IN already makes:
        // otherwise a text subject against an integer bound reaches the
        // evaluator, which indexes the empty text block of the wrong operand.
        if (bound.kind != expr_kind_t::literal && bound.result_type != subject) {
            m_errors.push_back(std::string("BETWEEN: bound of type ") + to_string(bound.result_type) +
                               " cannot be compared against " + to_string(subject));
            return data_type_t::unknown;
        }
        std::string reason;
        if (!coerce_literal(bound, subject, m_options, reason)) {
            m_errors.push_back("BETWEEN: " + reason);
            return data_type_t::unknown;
        }
    }
    return node.result_type = data_type_t::boolean;
}

merope::data_type_t merope::c_expr_compiler::compile_binary(expr_t& node) {
    expr_t& left  = *node.args[0];
    expr_t& right = *node.args[1];

    const data_type_t left_type = compile(left);
    if (left_type == data_type_t::unknown) return left_type;
    const data_type_t right_type = compile(right);
    if (right_type == data_type_t::unknown) return right_type;

    if (is_logical(node.op)) {
        if (left_type != data_type_t::boolean || right_type != data_type_t::boolean) {
            m_errors.push_back(std::string(to_string(node.op)) + " expects boolean operands");
            return data_type_t::unknown;
        }
        return node.result_type = data_type_t::boolean;
    }

    // A literal on either side adapts to the column it is compared with.
    std::string reason;
    if (right.kind == expr_kind_t::literal && left.kind != expr_kind_t::literal) {
        if (!coerce_literal(right, left_type, m_options, reason)) {
            m_errors.push_back(reason);
            return data_type_t::unknown;
        }
    } else if (left.kind == expr_kind_t::literal && right.kind != expr_kind_t::literal) {
        if (!coerce_literal(left, right_type, m_options, reason)) {
            m_errors.push_back(reason);
            return data_type_t::unknown;
        }
    }

    const data_type_t adjusted_left  = left.result_type;
    const data_type_t adjusted_right = right.result_type;

    if (is_comparison(node.op)) {
        const bool both_numeric  = is_numeric(adjusted_left) && is_numeric(adjusted_right);
        const bool both_text     = is_string_like(adjusted_left) && is_string_like(adjusted_right);
        const bool both_temporal = adjusted_left == adjusted_right && is_temporal(adjusted_left);
        const bool both_boolean  = adjusted_left == data_type_t::boolean &&
                                   adjusted_right == data_type_t::boolean;
        if (!both_numeric && !both_text && !both_temporal && !both_boolean) {
            m_errors.push_back(std::string("cannot compare ") + to_string(adjusted_left) +
                               " with " + to_string(adjusted_right));
            return data_type_t::unknown;
        }
        if (both_numeric && adjusted_left != adjusted_right &&
            (adjusted_left == data_type_t::decimal || adjusted_right == data_type_t::decimal)) {
            // Comparing scaled money against an unscaled number would be
            // off by a factor of 10000. Refuse rather than quietly rescale.
            m_errors.push_back("cannot compare a DECIMAL column with a non monetary value");
            return data_type_t::unknown;
        }
        return node.result_type = data_type_t::boolean;
    }

    if (!is_numeric(adjusted_left) || !is_numeric(adjusted_right)) {
        m_errors.push_back(std::string("arithmetic needs numeric operands, got ") +
                           to_string(adjusted_left) + " and " + to_string(adjusted_right));
        return data_type_t::unknown;
    }

    if (node.op == binary_op_t::divide) {
        // Always floating point: integer division silently truncating is a
        // classic source of wrong reports.
        return node.result_type = data_type_t::float64;
    }
    if (node.op == binary_op_t::multiply &&
        adjusted_left == data_type_t::decimal && adjusted_right == data_type_t::decimal) {
        m_errors.push_back("multiplying two DECIMAL columns would change the scale");
        return data_type_t::unknown;
    }
    if ((node.op == binary_op_t::add || node.op == binary_op_t::subtract) &&
        (adjusted_left == data_type_t::decimal) != (adjusted_right == data_type_t::decimal)) {
        m_errors.push_back("cannot add a DECIMAL and a non monetary value");
        return data_type_t::unknown;
    }
    return node.result_type = unify_numeric(adjusted_left, adjusted_right);
}

static bool aggregate_accepts(merope::aggregate_function_t function, merope::data_type_t type,
                              std::string& reason) {
    switch (function) {
    case merope::aggregate_function_t::count:
        return true;
    case merope::aggregate_function_t::sum:
    case merope::aggregate_function_t::average:
        if (!merope::is_numeric(type)) {
            reason = std::string(merope::to_string(function)) + " is not defined over " + merope::to_string(type);
            return false;
        }
        return true;
    case merope::aggregate_function_t::minimum:
    case merope::aggregate_function_t::maximum:
        if (type == merope::data_type_t::boolean || type == merope::data_type_t::unknown) {
            reason = std::string(merope::to_string(function)) + " is not defined over " + merope::to_string(type);
            return false;
        }
        return true;
    default:
        reason = "unknown aggregate function";
        return false;
    }
}

static merope::data_type_t aggregate_output_type(merope::aggregate_function_t function,
                                                  merope::data_type_t input) noexcept {
    switch (function) {
    case merope::aggregate_function_t::count:   return merope::data_type_t::int64;
    case merope::aggregate_function_t::sum:     return input;
    case merope::aggregate_function_t::average: return merope::data_type_t::float64;
    case merope::aggregate_function_t::minimum:
    case merope::aggregate_function_t::maximum: return input;
    default:                            return merope::data_type_t::unknown;
    }
}

std::vector<std::string> merope::physical_plan_t::result_columns() const {
    std::vector<std::string> names = group_by_names;
    if (aggregating()) {
        for (const aggregate_spec_t& spec : aggregates) names.push_back(spec.alias);
        return names;
    }
    return output_names;
}

std::vector<merope::data_type_t> merope::physical_plan_t::result_types() const {
    std::vector<data_type_t> types;
    if (aggregating()) {
        types.reserve(group_by.size() + aggregates.size());
        for (const slot_index_t slot : group_by) types.push_back(slot_types[slot]);
        for (const aggregate_spec_t& spec : aggregates) types.push_back(spec.output_type);
        return types;
    }
    types.reserve(output_slots.size());
    for (const slot_index_t slot : output_slots) types.push_back(slot_types[slot]);
    return types;
}

merope::validation_result_t merope::validate_plan(const query_plan_t& logical, const schema_t& schema) {
    validation_result_t result;
    physical_plan_t&    plan = result.plan;

    if (schema.columns.empty()) {
        result.errors.push_back("the dataset has no schema to validate against");
        return result;
    }

    c_slot_table          slots(schema);
    const parse_options_t options = schema.parse_options();
    c_expr_compiler       compiler(slots, options, result.errors);

    std::vector<expr_ptr> filters;
    bool                  seen_group_by = false;

    for (const operation_t& operation : logical.operations) {
        switch (operation.type) {
        case operation_type_t::project: {
            // A plain column list only pins down the output shape.
            for (const std::string& name : operation.columns) {
                const slot_index_t slot = slots.bind_column(name);
                if (slot == k_no_slot) {
                    result.errors.push_back("unknown column '" + name + "' in project");
                    continue;
                }
                plan.output_slots.push_back(slot);
                plan.output_names.push_back(name);
            }
            if (operation.expression_text.empty()) break;

            std::string parse_error;
            expr_ptr    expression = parse_expression(operation.expression_text, parse_error);
            if (expression == nullptr) {
                result.errors.push_back("project '" + operation.expression_text + "': " + parse_error);
                break;
            }
            const data_type_t type = compiler.compile(*expression);
            if (type == data_type_t::unknown) break;
            if (slots.lookup(operation.alias) != k_no_slot) {
                result.errors.push_back("alias '" + operation.alias + "' is already in use");
                break;
            }

            const slot_index_t slot = slots.add_computed(operation.alias, type);
            plan.computed.push_back(computed_column_t{operation.alias, std::move(expression), type});
            plan.output_slots.push_back(slot);
            plan.output_names.push_back(operation.alias);
            break;
        }

        case operation_type_t::filter: {
            std::string parse_error;
            expr_ptr    predicate = parse_expression(operation.predicate_text, parse_error);
            if (predicate == nullptr) {
                result.errors.push_back("filter '" + operation.predicate_text + "': " + parse_error);
                break;
            }
            const data_type_t type = compiler.compile(*predicate);
            if (type == data_type_t::unknown) break;
            if (type != data_type_t::boolean) {
                result.errors.push_back("filter '" + operation.predicate_text +
                                        "' is not a boolean predicate");
                break;
            }
            filters.push_back(std::move(predicate));
            break;
        }

        case operation_type_t::group_by: {
            seen_group_by = true;
            for (const std::string& name : operation.columns) {
                slot_index_t slot = slots.lookup(name);
                if (slot == k_no_slot) slot = slots.bind_column(name);
                if (slot == k_no_slot) {
                    result.errors.push_back("unknown column '" + name + "' in group_by");
                    continue;
                }
                if (slots.type_of(slot) == data_type_t::float64) {
                    // Grouping on a float compares bit patterns; almost never
                    // what was meant, and never reproducible across machines.
                    result.warnings.push_back("grouping on FLOAT64 column '" + name +
                                              "' compares exact values");
                }
                plan.group_by.push_back(slot);
                plan.group_by_names.push_back(name);
            }
            break;
        }

        case operation_type_t::aggregate: {
            aggregate_spec_t spec;
            spec.function = operation.function;
            spec.alias    = operation.alias;

            if (operation.column.empty()) {
                if (operation.function != aggregate_function_t::count) {
                    result.errors.push_back(std::string(to_string(operation.function)) +
                                            " needs a column");
                    break;
                }
                spec.input_slot  = k_no_slot;
                spec.input_type  = data_type_t::int64;
                spec.output_type = data_type_t::int64;
            } else {
                slot_index_t slot = slots.lookup(operation.column);
                if (slot == k_no_slot) slot = slots.bind_column(operation.column);
                if (slot == k_no_slot) {
                    result.errors.push_back("unknown column '" + operation.column + "' in aggregate");
                    break;
                }
                spec.input_slot = slot;
                spec.input_type = slots.type_of(slot);

                std::string reason;
                if (!aggregate_accepts(operation.function, spec.input_type, reason)) {
                    result.errors.push_back(reason + " (column '" + operation.column + "')");
                    break;
                }
                spec.output_type = aggregate_output_type(operation.function, spec.input_type);
            }
            plan.aggregates.push_back(std::move(spec));
            break;
        }

        case operation_type_t::sort:
            // Resolved after the output shape is known.
            break;

        case operation_type_t::limit:
            if (plan.limit != 0 && plan.limit != operation.limit_rows) {
                result.warnings.push_back("plan sets the limit more than once, using the smallest");
                plan.limit = std::min(plan.limit, operation.limit_rows);
            } else {
                plan.limit = operation.limit_rows;
            }
            break;
        }
    }

    if (!result.errors.empty()) return result;

    // Grouping without an aggregate is a distinct query; make that explicit
    // rather than guessing what the planner meant.
    if (seen_group_by && plan.aggregates.empty()) {
        aggregate_spec_t spec;
        spec.function    = aggregate_function_t::count;
        spec.alias       = "row_count";
        spec.input_slot  = k_no_slot;
        spec.output_type = data_type_t::int64;
        plan.aggregates.push_back(spec);
        result.warnings.push_back("group_by had no aggregate, added count(*) as row_count");
    }

    if (!plan.aggregates.empty() && plan.output_slots.size() > 0 && plan.group_by.empty()) {
        // Projected columns alongside a global aggregate have no defined value.
        result.warnings.push_back("projected columns are dropped: the plan aggregates without grouping");
        plan.output_slots.clear();
        plan.output_names.clear();
    }

    // A plan that scans nothing and computes nothing has no meaning.
    if (plan.aggregates.empty() && plan.output_slots.empty()) {
        for (std::size_t slot = 0; slot < slots.scanned_count(); ++slot) {
            plan.output_slots.push_back(slot);
            plan.output_names.push_back(schema.columns[slots.projection()[slot]].query_name());
        }
        if (plan.output_slots.empty()) {
            // Nothing was referenced at all; select every column.
            for (const column_schema_t& column : schema.columns) {
                const slot_index_t slot = slots.bind_column(column.query_name());
                plan.output_slots.push_back(slot);
                plan.output_names.push_back(column.query_name());
            }
            result.warnings.push_back("plan referenced no columns, selecting all of them");
        }
    }

    // Combine the filters with AND, in the order the plan listed them.
    for (expr_ptr& predicate : filters) {
        plan.filter = plan.filter == nullptr
                          ? std::move(predicate)
                          : make_binary(binary_op_t::logical_and, std::move(plan.filter),
                                        std::move(predicate));
    }
    if (plan.filter != nullptr) plan.filter->result_type = data_type_t::boolean;

    plan.projection = slots.projection();
    plan.slot_types = slots.all_types();

    // The projection is final now, so compile time slot numbers can be turned
    // into the positions the engine will actually find the values at.
    for (computed_column_t& computed : plan.computed) {
        if (computed.expression != nullptr) remap_slots(*computed.expression, slots);
    }
    if (plan.filter != nullptr) remap_slots(*plan.filter, slots);
    for (slot_index_t& slot : plan.group_by)     slot = slots.final_slot(slot);
    for (slot_index_t& slot : plan.output_slots) slot = slots.final_slot(slot);
    for (aggregate_spec_t& spec : plan.aggregates) {
        spec.input_slot = slots.final_slot(spec.input_slot);
    }

    // Sorting is over the result columns, so it is resolved last.
    const std::vector<std::string> result_names = plan.result_columns();
    for (const operation_t& operation : logical.operations) {
        if (operation.type != operation_type_t::sort) continue;
        const auto found = std::find_if(result_names.begin(), result_names.end(),
                                        [&](const std::string& name) {
                                            return lower_copy(name) == lower_copy(operation.column);
                                        });
        if (found == result_names.end()) {
            result.errors.push_back("sort column '" + operation.column +
                                    "' is not one of the result columns");
            continue;
        }
        sort_spec_t spec;
        spec.output_index = static_cast<std::size_t>(std::distance(result_names.begin(), found));
        spec.order        = operation.order;
        plan.sorts.push_back(spec);
    }

    if (!result.errors.empty()) return result;

    // A result set that is not provably small must be bounded, otherwise the
    // engine would stream an unlimited number of rows back to the caller.
    const bool provably_small = plan.aggregating() && plan.group_by.empty();
    if (plan.limit == 0 && !provably_small) {
        plan.limit = k_default_result_limit;
        result.warnings.push_back("plan had no limit on a potentially large result, applied limit " +
                                  std::to_string(k_default_result_limit));
    }

    result.accepted = true;
    return result;
}

std::string merope::format_physical_plan(const physical_plan_t& plan, const schema_t& schema) {
    std::ostringstream out;

    out << "  scan       ";
    for (std::size_t index = 0; index < plan.projection.size(); ++index) {
        if (index > 0) out << ", ";
        out << schema.columns[plan.projection[index]].query_name() << ":"
            << to_string(schema.columns[plan.projection[index]].physical_type);
    }
    if (plan.projection.empty()) out << "(nothing)";
    out << "\n";

    for (const computed_column_t& computed : plan.computed) {
        out << "  compute    " << computed.name << " = " << expression_to_string(*computed.expression)
            << " : " << to_string(computed.type) << "\n";
    }
    if (plan.filter != nullptr) {
        out << "  filter     " << expression_to_string(*plan.filter) << "\n";
    }
    if (!plan.group_by_names.empty()) {
        out << "  group by   ";
        for (std::size_t index = 0; index < plan.group_by_names.size(); ++index) {
            if (index > 0) out << ", ";
            out << plan.group_by_names[index];
        }
        out << "\n";
    }
    for (const aggregate_spec_t& spec : plan.aggregates) {
        out << "  aggregate  " << to_string(spec.function) << " -> " << spec.alias << " : "
            << to_string(spec.output_type);
        if (spec.needs_partial_sum_and_count()) out << "   (reduced as sum + count)";
        out << "\n";
    }
    for (const sort_spec_t& spec : plan.sorts) {
        out << "  sort       column " << spec.output_index << " " << to_string(spec.order) << "\n";
    }
    if (plan.limit > 0) out << "  limit      " << plan.limit << "\n";

    return out.str();
}

// ------------------------------------------------------------ slot table ---

merope::c_slot_table::c_slot_table(const schema_t& schema) : m_schema(schema) {}

bool merope::is_computed_slot(slot_index_t slot) noexcept {
    return slot != k_no_slot && (slot & k_computed_flag) != 0;
}

merope::slot_index_t merope::c_slot_table::bind_column(const std::string& name) {
    const auto existing = m_by_name.find(lower_copy(name));
    if (existing != m_by_name.end()) return existing->second;

    const std::size_t physical = m_schema.index_of(name);
    if (physical == k_invalid_column) return k_no_slot;

    // The same physical column can be reached by two names; reuse the slot.
    for (std::size_t slot = 0; slot < m_projection.size(); ++slot) {
        if (m_projection[slot] == physical) {
            m_by_name.emplace(lower_copy(name), slot);
            return slot;
        }
    }

    const slot_index_t slot = m_projection.size();
    m_projection.push_back(physical);
    m_scanned_types.push_back(m_schema.columns[physical].physical_type);
    m_by_name.emplace(lower_copy(name), slot);
    return slot;
}

merope::slot_index_t merope::c_slot_table::add_computed(const std::string& name, data_type_t type) {
    const slot_index_t slot = k_computed_flag | m_computed_types.size();
    m_computed_types.push_back(type);
    m_by_name[lower_copy(name)] = slot;
    return slot;
}

merope::slot_index_t merope::c_slot_table::lookup(const std::string& name) const {
    const auto found = m_by_name.find(lower_copy(name));
    return found == m_by_name.end() ? k_no_slot : found->second;
}

merope::data_type_t merope::c_slot_table::type_of(slot_index_t slot) const {
    return is_computed_slot(slot) ? m_computed_types[slot & ~k_computed_flag]
                                  : m_scanned_types[slot];
}

merope::slot_index_t merope::c_slot_table::final_slot(slot_index_t slot) const noexcept {
    if (slot == k_no_slot) return k_no_slot;
    return is_computed_slot(slot) ? m_projection.size() + (slot & ~k_computed_flag) : slot;
}

std::vector<merope::data_type_t> merope::c_slot_table::all_types() const {
    std::vector<data_type_t> types = m_scanned_types;
    types.insert(types.end(), m_computed_types.begin(), m_computed_types.end());
    return types;
}
