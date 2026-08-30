#include "ai_provider.h"

#include "../core/json.h"
#include "mock_ai_provider.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <unordered_map>

static std::string lower_copy(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

static std::vector<std::string> words_of(const std::string& text) {
    std::vector<std::string> words;
    std::string              current;
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        if (std::isalnum(ch) != 0 || raw == '_') {
            current.push_back(static_cast<char>(std::tolower(ch)));
        } else if (!current.empty()) {
            words.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) words.push_back(current);
    return words;
}

// Conventional names a model would reach for when the file has no header.
// Deliberately a small fixed table: the mock must not look cleverer than it is.
static std::string conventional_name(merope::semantic_type_t type, std::size_t ordinal) {
    switch (type) {
    case merope::semantic_type_t::identifier:
        return ordinal == 0 ? "transaction_id" : (ordinal == 1 ? "user_id" : "entity_id");
    case merope::semantic_type_t::monetary:   return ordinal == 0 ? "amount" : "amount_2";
    case merope::semantic_type_t::country:    return ordinal == 0 ? "country" : "country_2";
    case merope::semantic_type_t::timestamp:  return ordinal == 0 ? "timestamp" : "timestamp_2";
    case merope::semantic_type_t::date_value: return ordinal == 0 ? "date" : "date_2";
    case merope::semantic_type_t::quantity:   return ordinal == 0 ? "quantity" : "quantity_2";
    case merope::semantic_type_t::category:   return ordinal == 0 ? "category" : "category_2";
    case merope::semantic_type_t::status:     return ordinal == 0 ? "status" : "status_2";
    case merope::semantic_type_t::email:      return "email";
    case merope::semantic_type_t::percentage: return "percentage";
    case merope::semantic_type_t::flag:       return ordinal == 0 ? "flag" : "flag_2";
    case merope::semantic_type_t::text:       return ordinal == 0 ? "description" : "text_2";
    case merope::semantic_type_t::unknown:
    default:                          return std::string();
    }
}

// Finds the column a phrase refers to, by semantic name, physical name, or a
// word that appears in either. Returns empty when nothing matches cleanly.
static std::string resolve_column(const merope::schema_t& schema, const std::vector<std::string>& words,
                                  std::size_t from, std::size_t to) {
    // The word closest to the keyword wins. "total amount by country" must
    // resolve the SUM to amount, not to the country mentioned three words later.
    std::string best;
    std::size_t best_index = words.size();

    for (const merope::column_schema_t& column : schema.columns) {
        for (const std::string& candidate : {column.semantic_name, column.physical_name}) {
            if (candidate.empty()) continue;
            const std::string key = lower_copy(candidate);
            for (std::size_t index = from; index < to && index < words.size(); ++index) {
                if (words[index] == key && index < best_index) {
                    best       = column.query_name();
                    best_index = index;
                }
            }
        }
    }
    if (!best.empty()) return best;

    // Fall back to a word that is a component of a column name, e.g. "revenue"
    // matching "revenue_eur".
    for (const merope::column_schema_t& column : schema.columns) {
        const std::string key = lower_copy(column.query_name());
        for (std::size_t index = from; index < to && index < words.size(); ++index) {
            if (words[index].size() >= 4 && key.find(words[index]) != std::string::npos &&
                index < best_index) {
                best       = column.query_name();
                best_index = index;
            }
        }
    }
    return best;
}

static const merope::column_schema_t* first_of_semantic(const merope::schema_t& schema, merope::semantic_type_t type) {
    for (const merope::column_schema_t& column : schema.columns) {
        if (column.semantic_type == type) return &column;
    }
    return nullptr;
}

static const merope::column_schema_t* first_temporal(const merope::schema_t& schema) {
    for (const merope::column_schema_t& column : schema.columns) {
        if (merope::is_temporal(column.physical_type)) return &column;
    }
    return nullptr;
}

static const merope::column_schema_t* first_measure(const merope::schema_t& schema) {
    if (const merope::column_schema_t* money = first_of_semantic(schema, merope::semantic_type_t::monetary)) {
        return money;
    }
    for (const merope::column_schema_t& column : schema.columns) {
        if (merope::is_numeric(column.physical_type) && column.semantic_type != merope::semantic_type_t::identifier) {
            return &column;
        }
    }
    return nullptr;
}

const char* merope::c_mock_ai_provider::name() const noexcept { return "mock"; }

merope::schema_inference_t merope::c_mock_ai_provider::infer_schema(const dataset_profile_t& profile) {
    schema_inference_t inference;
    inference.provider = name();
    inference.proposals = infer_heuristically(profile);
    inference.notes.push_back(
        "mock provider: proposals come from the deterministic heuristics, with conventional "
        "names filled in where the file has no header");

    if (profile.dialect.has_header) {
        inference.notes.push_back("file has a header, keeping the original column labels");
        return inference;
    }

    std::unordered_map<int, std::size_t> ordinals;
    for (inference_hint_t& hint : inference.proposals.hints) {
        if (hint.semantic_type == semantic_type_t::unknown) {
            // Refusing to name a column is a legal answer and is kept as one.
            hint.semantic_name = "column_" + std::to_string(hint.physical_index);
            hint.rationale     = "not enough evidence to name this column: " + hint.rationale;
            continue;
        }
        const std::size_t ordinal = ordinals[static_cast<int>(hint.semantic_type)]++;
        std::string       proposed = conventional_name(hint.semantic_type, ordinal);
        if (!proposed.empty()) {
            hint.semantic_name = proposed;
            hint.rationale += "; named by convention for " + std::string(to_string(hint.semantic_type));
        }
    }

    if (inference.has_unknown_columns()) {
        inference.notes.push_back("some columns were left UNKNOWN and need a decision from you");
    }
    return inference;
}

merope::query_plan_t merope::c_mock_ai_provider::generate_query_plan(const schema_t& schema,
                                                                       const std::string& query) {
    query_plan_t plan;
    plan.natural_language_query = query;
    plan.provider               = name();

    const std::vector<std::string> words = words_of(query);

    // 1. A four digit year anywhere in the question becomes a filter on the
    //    temporal column, expressed as year(col) so the engine can compute it.
    std::string year_literal;
    for (const std::string& word : words) {
        if (word.size() == 4 && std::all_of(word.begin(), word.end(), [](char c) {
                return std::isdigit(static_cast<unsigned char>(c)) != 0;
            })) {
            const int value = std::atoi(word.c_str());
            if (value >= 1900 && value <= 2100) year_literal = word;
        }
    }
    const column_schema_t* temporal = first_temporal(schema);
    if (!year_literal.empty() && temporal != nullptr) {
        operation_t project;
        project.type            = operation_type_t::project;
        project.expression_text = "year(" + temporal->query_name() + ")";
        project.alias           = "year";
        plan.operations.push_back(project);

        operation_t filter;
        filter.type           = operation_type_t::filter;
        filter.predicate_text = "year == " + year_literal;
        plan.operations.push_back(filter);
    }

    // 2. Grouping: "by <column>" or "per <column>".
    std::string group_column;
    for (std::size_t index = 0; index + 1 < words.size(); ++index) {
        if (words[index] == "by" || words[index] == "per" || words[index] == "podla") {
            group_column = resolve_column(schema, words, index + 1, index + 4);
            if (!group_column.empty()) break;
        }
    }
    if (!group_column.empty()) {
        operation_t group;
        group.type = operation_type_t::group_by;
        group.columns.push_back(group_column);
        plan.operations.push_back(group);
    }

    // 3. The aggregate the question is asking for.
    struct pattern_t { const char* word; aggregate_function_t function; };
    static constexpr pattern_t k_patterns[] = {
        {"total", aggregate_function_t::sum},     {"sum", aggregate_function_t::sum},
        {"revenue", aggregate_function_t::sum},   {"average", aggregate_function_t::average},
        {"avg", aggregate_function_t::average},   {"mean", aggregate_function_t::average},
        {"minimum", aggregate_function_t::minimum}, {"min", aggregate_function_t::minimum},
        {"maximum", aggregate_function_t::maximum}, {"max", aggregate_function_t::maximum},
        {"count", aggregate_function_t::count},   {"how", aggregate_function_t::count},
        {"number", aggregate_function_t::count}
    };

    bool                 found_aggregate = false;
    aggregate_function_t function        = aggregate_function_t::count;
    std::size_t          keyword_at      = 0;
    for (std::size_t index = 0; index < words.size() && !found_aggregate; ++index) {
        for (const pattern_t& pattern : k_patterns) {
            if (words[index] == pattern.word) {
                function        = pattern.function;
                keyword_at      = index;
                found_aggregate = true;
                break;
            }
        }
    }

    if (found_aggregate) {
        operation_t aggregate;
        aggregate.type     = operation_type_t::aggregate;
        aggregate.function = function;

        if (function != aggregate_function_t::count) {
            aggregate.column = resolve_column(schema, words, keyword_at, keyword_at + 4);
            if (aggregate.column.empty()) {
                const column_schema_t* measure = first_measure(schema);
                if (measure != nullptr) aggregate.column = measure->query_name();
            }
            if (aggregate.column.empty()) {
                // No measure to aggregate: fall back to counting rows rather
                // than inventing a column that does not exist.
                aggregate.function = aggregate_function_t::count;
                aggregate.alias    = "row_count";
            } else {
                aggregate.alias = std::string(to_string(aggregate.function)) + "_" + aggregate.column;
            }
        } else {
            aggregate.alias = "row_count";
        }
        plan.operations.push_back(aggregate);

        operation_t sort;
        sort.type   = operation_type_t::sort;
        sort.column = aggregate.alias;
        sort.order  = sort_order_t::descending;
        plan.operations.push_back(sort);
    } else if (!group_column.empty()) {
        operation_t aggregate;
        aggregate.type     = operation_type_t::aggregate;
        aggregate.function = aggregate_function_t::count;
        aggregate.alias    = "row_count";
        plan.operations.push_back(aggregate);
    } else {
        // No aggregate and no grouping: this is a row listing.
        operation_t project;
        project.type = operation_type_t::project;
        for (const column_schema_t& column : schema.columns) {
            project.columns.push_back(column.query_name());
        }
        plan.operations.push_back(project);
    }

    // 4. An explicit row cap if the question asks for one.
    std::int64_t limit = 0;
    for (std::size_t index = 0; index + 1 < words.size(); ++index) {
        if (words[index] == "top" || words[index] == "first" || words[index] == "limit") {
            const std::string& next = words[index + 1];
            if (std::all_of(next.begin(), next.end(), [](char c) {
                    return std::isdigit(static_cast<unsigned char>(c)) != 0;
                })) {
                limit = std::atoll(next.c_str());
            }
        }
    }
    if (limit > 0) {
        operation_t cap;
        cap.type       = operation_type_t::limit;
        cap.limit_rows = limit;
        plan.operations.push_back(cap);
    }

    return plan;
}

bool merope::schema_inference_t::has_unknown_columns() const noexcept {
    return std::any_of(proposals.hints.begin(), proposals.hints.end(),
                       [](const inference_hint_t& hint) {
                           return hint.semantic_type == semantic_type_t::unknown;
                       });
}

std::string merope::build_schema_prompt(const dataset_profile_t& profile) {
    // Only the profile and a handful of example values travel; the dataset
    // itself never leaves the process.
    json_value_t root = json_value_t::make_object();
    root.set("task", json_value_t::make_string(
                         "Propose a semantic name and type for each column. Answer UNKNOWN when the "
                         "evidence is insufficient. Do not invent column meanings."));
    root.set("has_header", json_value_t::make_bool(profile.dialect.has_header));
    root.set("sample_rows", json_value_t::make_number(static_cast<double>(profile.sample_rows)));
    root.set("sample_is_estimate", json_value_t::make_bool(!profile.exact));

    json_value_t columns = json_value_t::make_array();
    for (const column_profile_t& stats : profile.columns) {
        json_value_t entry = json_value_t::make_object();
        entry.set("physical_index", json_value_t::make_number(static_cast<double>(stats.physical_index)));
        entry.set("physical_name", json_value_t::make_string(stats.physical_name));
        entry.set("type", json_value_t::make_string(to_string(stats.inferred_type)));
        entry.set("null_count", json_value_t::make_number(static_cast<double>(stats.null_count)));
        entry.set("unique_count", json_value_t::make_number(static_cast<double>(stats.unique_count)));
        entry.set("unique_ratio", json_value_t::make_number(stats.unique_ratio));
        if (stats.has_min) {
            entry.set("min", json_value_t::make_string(cell_to_display(stats.min_value, stats.inferred_type)));
        }
        if (stats.has_max) {
            entry.set("max", json_value_t::make_string(cell_to_display(stats.max_value, stats.inferred_type)));
        }
        json_value_t examples = json_value_t::make_array();
        for (const std::string& example : stats.examples) {
            examples.array_value.push_back(json_value_t::make_string(example));
        }
        entry.set("examples", std::move(examples));
        columns.array_value.push_back(std::move(entry));
    }
    root.set("columns", std::move(columns));
    return json_serialize(root, 2);
}

std::string merope::build_plan_prompt(const schema_t& schema, const std::string& query) {
    json_value_t root = json_value_t::make_object();
    root.set("task", json_value_t::make_string(
                         "Translate the question into a plan using only the listed operations and "
                         "columns. Return JSON only. Do not return code."));
    root.set("question", json_value_t::make_string(query));

    json_value_t operations = json_value_t::make_array();
    for (const char* allowed : {"project", "filter", "group_by", "aggregate", "sort", "limit"}) {
        operations.array_value.push_back(json_value_t::make_string(allowed));
    }
    root.set("allowed_operations", std::move(operations));

    json_value_t functions = json_value_t::make_array();
    for (const char* allowed : {"count", "sum", "avg", "min", "max"}) {
        functions.array_value.push_back(json_value_t::make_string(allowed));
    }
    root.set("allowed_aggregates", std::move(functions));

    json_value_t columns = json_value_t::make_array();
    for (const column_schema_t& column : schema.columns) {
        json_value_t entry = json_value_t::make_object();
        entry.set("name", json_value_t::make_string(column.query_name()));
        entry.set("type", json_value_t::make_string(to_string(column.physical_type)));
        entry.set("meaning", json_value_t::make_string(to_string(column.semantic_type)));
        columns.array_value.push_back(std::move(entry));
    }
    root.set("columns", std::move(columns));
    return json_serialize(root, 2);
}

std::unique_ptr<merope::c_ai_provider> merope::make_mock_provider() {
    return std::make_unique<c_mock_ai_provider>();
}

