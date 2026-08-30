#include "query_plan.h"

#include <algorithm>
#include <cctype>
#include <sstream>

static std::string lower_copy(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

static bool read_string_array(const merope::json_value_t& value, std::vector<std::string>& out, std::string& error) {
    if (!value.is_array()) {
        error = "expected an array of column names";
        return false;
    }
    for (const merope::json_value_t& item : value.array_value) {
        if (!item.is_string()) {
            error = "column names must be strings";
            return false;
        }
        out.push_back(item.string_value);
    }
    return true;
}

const char* merope::to_string(operation_type_t type) noexcept {
    switch (type) {
    case operation_type_t::project:   return "project";
    case operation_type_t::filter:    return "filter";
    case operation_type_t::group_by:  return "group_by";
    case operation_type_t::aggregate: return "aggregate";
    case operation_type_t::sort:      return "sort";
    case operation_type_t::limit:     return "limit";
    default:                          return "?";
    }
}

const char* merope::to_string(aggregate_function_t function) noexcept {
    switch (function) {
    case aggregate_function_t::count:   return "count";
    case aggregate_function_t::sum:     return "sum";
    case aggregate_function_t::average: return "avg";
    case aggregate_function_t::minimum: return "min";
    case aggregate_function_t::maximum: return "max";
    default:                            return "?";
    }
}

const char* merope::to_string(sort_order_t order) noexcept {
    return order == sort_order_t::descending ? "desc" : "asc";
}

bool merope::operation_type_from_string(const std::string& name, operation_type_t& out) noexcept {
    const std::string key = lower_copy(name);
    // "map" and "compute" are accepted spellings of a computed projection.
    if (key == "project" || key == "map" || key == "compute") { out = operation_type_t::project;   return true; }
    if (key == "filter" || key == "where")                    { out = operation_type_t::filter;    return true; }
    if (key == "group_by" || key == "groupby")                { out = operation_type_t::group_by;  return true; }
    if (key == "aggregate" || key == "agg")                   { out = operation_type_t::aggregate; return true; }
    if (key == "sort" || key == "order_by")                   { out = operation_type_t::sort;      return true; }
    if (key == "limit" || key == "top")                       { out = operation_type_t::limit;     return true; }
    return false;
}

bool merope::aggregate_function_from_string(const std::string& name, aggregate_function_t& out) noexcept {
    const std::string key = lower_copy(name);
    if (key == "count")                  { out = aggregate_function_t::count;   return true; }
    if (key == "sum")                    { out = aggregate_function_t::sum;     return true; }
    if (key == "avg" || key == "average"){ out = aggregate_function_t::average; return true; }
    if (key == "min" || key == "minimum"){ out = aggregate_function_t::minimum; return true; }
    if (key == "max" || key == "maximum"){ out = aggregate_function_t::maximum; return true; }
    return false;
}

bool merope::parse_query_plan(const json_value_t& root, query_plan_t& out, std::string& error) {
    out = query_plan_t{};
    if (!root.is_object()) {
        error = "plan must be a JSON object";
        return false;
    }

    out.natural_language_query = root.string_or("query", "");
    out.provider               = root.string_or("provider", "");

    const json_value_t* operations = root.find("operations");
    if (operations == nullptr || !operations->is_array()) {
        error = "plan has no operations array";
        return false;
    }
    if (operations->array_value.empty()) {
        error = "plan has no operations";
        return false;
    }

    for (std::size_t index = 0; index < operations->array_value.size(); ++index) {
        const json_value_t& entry  = operations->array_value[index];
        const std::string   prefix = "operation " + std::to_string(index) + ": ";
        if (!entry.is_object()) {
            error = prefix + "not an object";
            return false;
        }

        operation_t operation;
        const std::string type_name = entry.string_or("type", "");
        if (!operation_type_from_string(type_name, operation.type)) {
            error = prefix + "unknown operation type '" + type_name + "'";
            return false;
        }

        switch (operation.type) {
        case operation_type_t::project: {
            operation.expression_text = entry.string_or("expr", entry.string_or("expression", ""));
            operation.alias           = entry.string_or("as", entry.string_or("alias", ""));
            if (const json_value_t* columns = entry.find("columns"); columns != nullptr) {
                std::string reason;
                if (!read_string_array(*columns, operation.columns, reason)) {
                    error = prefix + reason;
                    return false;
                }
            }
            if (operation.expression_text.empty() && operation.columns.empty()) {
                error = prefix + "project needs either expr or columns";
                return false;
            }
            if (!operation.expression_text.empty() && operation.alias.empty()) {
                error = prefix + "a computed projection needs an alias (as)";
                return false;
            }
            break;
        }
        case operation_type_t::filter: {
            operation.predicate_text =
                entry.string_or("predicate", entry.string_or("expr", entry.string_or("condition", "")));
            if (operation.predicate_text.empty()) {
                error = prefix + "filter needs a predicate";
                return false;
            }
            break;
        }
        case operation_type_t::group_by: {
            const json_value_t* columns = entry.find("columns");
            if (columns == nullptr) {
                const std::string single = entry.string_or("column", "");
                if (single.empty()) {
                    error = prefix + "group_by needs columns";
                    return false;
                }
                operation.columns.push_back(single);
            } else {
                std::string reason;
                if (!read_string_array(*columns, operation.columns, reason)) {
                    error = prefix + reason;
                    return false;
                }
            }
            if (operation.columns.empty()) {
                error = prefix + "group_by needs at least one column";
                return false;
            }
            break;
        }
        case operation_type_t::aggregate: {
            const std::string function_name = entry.string_or("function", entry.string_or("fn", ""));
            if (!aggregate_function_from_string(function_name, operation.function)) {
                error = prefix + "unknown aggregate function '" + function_name + "'";
                return false;
            }
            operation.column = entry.string_or("column", "");
            operation.alias  = entry.string_or("as", entry.string_or("alias", ""));
            if (operation.function != aggregate_function_t::count && operation.column.empty()) {
                error = prefix + std::string(to_string(operation.function)) + " needs a column";
                return false;
            }
            if (operation.alias.empty()) {
                operation.alias = std::string(to_string(operation.function)) +
                                  (operation.column.empty() ? "" : "_" + operation.column);
            }
            break;
        }
        case operation_type_t::sort: {
            operation.column = entry.string_or("column", entry.string_or("by", ""));
            if (operation.column.empty()) {
                error = prefix + "sort needs a column";
                return false;
            }
            const std::string order = lower_copy(entry.string_or("order", "asc"));
            if (order == "desc" || order == "descending") {
                operation.order = sort_order_t::descending;
            } else if (order == "asc" || order == "ascending") {
                operation.order = sort_order_t::ascending;
            } else {
                error = prefix + "sort order must be asc or desc";
                return false;
            }
            break;
        }
        case operation_type_t::limit: {
            operation.limit_rows = entry.int_or("n", entry.int_or("rows", entry.int_or("limit", 0)));
            if (operation.limit_rows <= 0) {
                error = prefix + "limit must be a positive number of rows";
                return false;
            }
            break;
        }
        }

        out.operations.push_back(std::move(operation));
    }

    return true;
}

bool merope::parse_query_plan(const std::string& text, query_plan_t& out, std::string& error) {
    json_value_t root;
    if (!json_parse(text, root, error)) return false;
    return parse_query_plan(root, out, error);
}

merope::json_value_t merope::plan_to_json(const query_plan_t& plan) {
    json_value_t root = json_value_t::make_object();
    if (!plan.natural_language_query.empty()) {
        root.set("query", json_value_t::make_string(plan.natural_language_query));
    }
    if (!plan.provider.empty()) {
        root.set("provider", json_value_t::make_string(plan.provider));
    }

    json_value_t operations = json_value_t::make_array();
    for (const operation_t& operation : plan.operations) {
        json_value_t entry = json_value_t::make_object();
        entry.set("type", json_value_t::make_string(to_string(operation.type)));

        switch (operation.type) {
        case operation_type_t::project:
            if (!operation.expression_text.empty()) {
                entry.set("expr", json_value_t::make_string(operation.expression_text));
                entry.set("as", json_value_t::make_string(operation.alias));
            }
            if (!operation.columns.empty()) {
                json_value_t columns = json_value_t::make_array();
                for (const std::string& name : operation.columns) {
                    columns.array_value.push_back(json_value_t::make_string(name));
                }
                entry.set("columns", std::move(columns));
            }
            break;
        case operation_type_t::filter:
            entry.set("predicate", json_value_t::make_string(operation.predicate_text));
            break;
        case operation_type_t::group_by: {
            json_value_t columns = json_value_t::make_array();
            for (const std::string& name : operation.columns) {
                columns.array_value.push_back(json_value_t::make_string(name));
            }
            entry.set("columns", std::move(columns));
            break;
        }
        case operation_type_t::aggregate:
            entry.set("function", json_value_t::make_string(to_string(operation.function)));
            if (!operation.column.empty()) {
                entry.set("column", json_value_t::make_string(operation.column));
            }
            entry.set("as", json_value_t::make_string(operation.alias));
            break;
        case operation_type_t::sort:
            entry.set("column", json_value_t::make_string(operation.column));
            entry.set("order", json_value_t::make_string(to_string(operation.order)));
            break;
        case operation_type_t::limit:
            entry.set("n", json_value_t::make_number(static_cast<double>(operation.limit_rows)));
            break;
        }

        operations.array_value.push_back(std::move(entry));
    }
    root.set("operations", std::move(operations));
    return root;
}

std::string merope::format_plan(const query_plan_t& plan) {
    std::ostringstream out;
    for (const operation_t& operation : plan.operations) {
        out << "  " << to_string(operation.type) << " ";
        switch (operation.type) {
        case operation_type_t::project:
            if (!operation.expression_text.empty()) {
                out << operation.expression_text << " as " << operation.alias;
            }
            if (!operation.columns.empty()) {
                for (std::size_t index = 0; index < operation.columns.size(); ++index) {
                    if (index > 0) out << ", ";
                    out << operation.columns[index];
                }
            }
            break;
        case operation_type_t::filter:
            out << operation.predicate_text;
            break;
        case operation_type_t::group_by:
            for (std::size_t index = 0; index < operation.columns.size(); ++index) {
                if (index > 0) out << ", ";
                out << operation.columns[index];
            }
            break;
        case operation_type_t::aggregate:
            out << to_string(operation.function) << "(" << (operation.column.empty() ? "*" : operation.column)
                << ") as " << operation.alias;
            break;
        case operation_type_t::sort:
            out << operation.column << " " << to_string(operation.order);
            break;
        case operation_type_t::limit:
            out << operation.limit_rows;
            break;
        }
        out << "\n";
    }
    return out.str();
}

