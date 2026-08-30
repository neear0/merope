#include "heuristic_inference.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <unordered_map>

// Turns a header label into a usable identifier: lowercase, separators folded
// to underscores, no leading digit.
static std::string normalise_name(const std::string& label) {
    std::string out;
    out.reserve(label.size());
    bool last_underscore = false;
    for (const char raw : label) {
        const auto ch = static_cast<unsigned char>(raw);
        if (std::isalnum(ch) != 0) {
            out.push_back(static_cast<char>(std::tolower(ch)));
            last_underscore = false;
        } else if (!out.empty() && !last_underscore) {
            out.push_back('_');
            last_underscore = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out.front())) != 0) {
        out.insert(out.begin(), 'c');
    }
    return out;
}

static std::vector<std::string> tokenise(const std::string& name) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char ch : name) {
        if (ch == '_') {
            if (!current.empty()) tokens.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

static bool has_token(const std::vector<std::string>& tokens, std::string_view word) {
    return std::find(tokens.begin(), tokens.end(), word) != tokens.end();
}

static bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

// What the header label alone suggests. UNKNOWN when the label says nothing.
static merope::semantic_type_t semantic_from_label(const std::string& name) {
    const std::vector<std::string> tokens = tokenise(name);

    if (has_token(tokens, "id") || contains(name, "_id") || name == "id" ||
        contains(name, "uuid") || contains(name, "guid") || contains(name, "key")) {
        return merope::semantic_type_t::identifier;
    }
    if (contains(name, "price") || contains(name, "amount") || contains(name, "revenue") ||
        contains(name, "cost") || contains(name, "total") || contains(name, "salary") ||
        contains(name, "fee") || contains(name, "balance") || contains(name, "suma") ||
        contains(name, "cena")) {
        return merope::semantic_type_t::monetary;
    }
    if (contains(name, "country") || contains(name, "krajina") || name == "iso" ||
        contains(name, "country_code")) {
        return merope::semantic_type_t::country;
    }
    if (contains(name, "email") || contains(name, "mail")) return merope::semantic_type_t::email;
    if (contains(name, "percent") || contains(name, "ratio") || contains(name, "rate")) {
        return merope::semantic_type_t::percentage;
    }
    if (contains(name, "timestamp") || contains(name, "datetime") || contains(name, "_at") ||
        contains(name, "time")) {
        return merope::semantic_type_t::timestamp;
    }
    if (contains(name, "date") || contains(name, "datum")) return merope::semantic_type_t::date_value;
    if (contains(name, "status") || contains(name, "state") || contains(name, "stav")) {
        return merope::semantic_type_t::status;
    }
    if (contains(name, "category") || contains(name, "type") || contains(name, "kind") ||
        contains(name, "kategoria")) {
        return merope::semantic_type_t::category;
    }
    if (contains(name, "count") || contains(name, "qty") || contains(name, "quantity") ||
        contains(name, "num") || contains(name, "pocet")) {
        return merope::semantic_type_t::quantity;
    }
    if (contains(name, "is_") || contains(name, "has_") || contains(name, "flag")) {
        return merope::semantic_type_t::flag;
    }
    return merope::semantic_type_t::unknown;
}

// What the data shape alone suggests, with the confidence that shape earns.
static void semantic_from_shape(const merope::column_profile_t& stats, merope::semantic_type_t& type, double& confidence,
                                std::string& rationale) {
    type       = merope::semantic_type_t::unknown;
    confidence = 0.0;
    rationale.clear();

    switch (stats.inferred_type) {
    case merope::data_type_t::decimal:
        type       = merope::semantic_type_t::monetary;
        confidence = stats.currency_ratio > 0.10 ? 0.90 : 0.75;
        rationale  = stats.currency_ratio > 0.10 ? "currency marked decimal values"
                                                 : "decimal values in monetary form";
        return;
    case merope::data_type_t::date:
        type       = merope::semantic_type_t::date_value;
        confidence = 0.90;
        rationale  = std::string("values match ") + merope::to_string(stats.date_pattern);
        return;
    case merope::data_type_t::datetime:
        type       = merope::semantic_type_t::timestamp;
        confidence = 0.90;
        rationale  = "values parse as date and time";
        return;
    case merope::data_type_t::boolean:
        type       = merope::semantic_type_t::flag;
        confidence = 0.85;
        rationale  = "two valued boolean column";
        return;
    case merope::data_type_t::int64:
        // High cardinality non negative integers are the classic surrogate key.
        if (stats.unique_ratio >= 0.95 && stats.all_non_negative && stats.null_count == 0) {
            type       = merope::semantic_type_t::identifier;
            confidence = 0.70;
            rationale  = "integer, unique ratio " + std::to_string(stats.unique_ratio).substr(0, 4) +
                         ", no nulls";
        } else {
            type       = merope::semantic_type_t::quantity;
            confidence = 0.55;
            rationale  = "integer with repeating values";
        }
        return;
    case merope::data_type_t::float64:
        if (stats.percentage_ratio >= 0.90) {
            type       = merope::semantic_type_t::percentage;
            confidence = 0.85;
            rationale  = "values carry a percent sign";
        } else {
            type       = merope::semantic_type_t::quantity;
            confidence = 0.55;
            rationale  = "continuous numeric values";
        }
        return;
    case merope::data_type_t::categorical:
        if (stats.country_ratio >= 0.95 && stats.unique_count <= 300) {
            type       = merope::semantic_type_t::country;
            confidence = 0.85;
            rationale  = "low cardinality two letter uppercase codes";
        } else {
            type       = merope::semantic_type_t::category;
            confidence = 0.60;
            rationale  = "low cardinality strings (" + std::to_string(stats.unique_count) + " distinct)";
        }
        return;
    case merope::data_type_t::utf8:
        if (stats.email_ratio >= 0.90) {
            type       = merope::semantic_type_t::email;
            confidence = 0.90;
            rationale  = "values have the shape of an email address";
        } else if (stats.country_ratio >= 0.95) {
            type       = merope::semantic_type_t::country;
            confidence = 0.70;
            rationale  = "two letter uppercase codes";
        } else {
            type       = merope::semantic_type_t::text;
            confidence = 0.50;
            rationale  = "free text";
        }
        return;
    case merope::data_type_t::unknown:
    default:
        return;
    }
}

// A default semantic name for a headerless file. The AI is expected to do
// better than this; the point is to never leave a column unaddressable.
static std::string default_name(merope::semantic_type_t type, std::size_t physical_index) {
    switch (type) {
    case merope::semantic_type_t::identifier: return "id";
    case merope::semantic_type_t::monetary:   return "amount";
    case merope::semantic_type_t::country:    return "country";
    case merope::semantic_type_t::email:      return "email";
    case merope::semantic_type_t::percentage: return "percentage";
    case merope::semantic_type_t::timestamp:  return "timestamp";
    case merope::semantic_type_t::date_value: return "date";
    case merope::semantic_type_t::status:     return "status";
    case merope::semantic_type_t::category:   return "category";
    case merope::semantic_type_t::quantity:   return "quantity";
    case merope::semantic_type_t::flag:       return "flag";
    case merope::semantic_type_t::text:       return "text";
    case merope::semantic_type_t::unknown:
    default:                          return "column_" + std::to_string(physical_index);
    }
}

// Do the label and the data disagree in a way that cannot be reconciled?
static bool conflicting(merope::semantic_type_t label, merope::semantic_type_t shape, const merope::column_profile_t& stats) {
    if (label == merope::semantic_type_t::unknown || shape == merope::semantic_type_t::unknown) return false;
    if (label == shape) return false;

    // A label naming money over text or over a date is a real contradiction.
    if (label == merope::semantic_type_t::monetary && !merope::is_numeric(stats.inferred_type)) return true;
    if (label == merope::semantic_type_t::country && !merope::is_string_like(stats.inferred_type)) return true;
    if ((label == merope::semantic_type_t::date_value || label == merope::semantic_type_t::timestamp) &&
        !merope::is_temporal(stats.inferred_type)) {
        return true;
    }
    if (label == merope::semantic_type_t::email && !merope::is_string_like(stats.inferred_type)) return true;
    if (label == merope::semantic_type_t::identifier &&
        (stats.inferred_type == merope::data_type_t::boolean || merope::is_temporal(stats.inferred_type))) {
        return true;
    }
    return false;
}

merope::heuristic_result_t merope::infer_heuristically(const dataset_profile_t& profile) {
    heuristic_result_t result;
    result.hints.reserve(profile.columns.size());

    std::unordered_map<std::string, int> name_uses;

    for (const column_profile_t& stats : profile.columns) {
        inference_hint_t hint;
        hint.physical_index = stats.physical_index;

        semantic_type_t shape_type = semantic_type_t::unknown;
        double          shape_conf = 0.0;
        std::string     shape_why;
        semantic_from_shape(stats, shape_type, shape_conf, shape_why);

        const std::string label = profile.dialect.has_header ? normalise_name(stats.physical_name)
                                                             : std::string();
        const semantic_type_t label_type =
            label.empty() ? semantic_type_t::unknown : semantic_from_label(label);

        if (conflicting(label_type, shape_type, stats)) {
            // The header promises one thing and the bytes say another. Refusing
            // to pick is the honest answer; the user or the AI decides.
            hint.semantic_type = semantic_type_t::unknown;
            hint.confidence    = 0.20;
            hint.rationale     = "label suggests " + std::string(to_string(label_type)) +
                                 " but the data is " + to_string(stats.inferred_type) +
                                 " (" + shape_why + ")";
            hint.semantic_name = label;
            result.hints.push_back(std::move(hint));
            continue;
        }

        if (label_type != semantic_type_t::unknown && label_type == shape_type) {
            hint.semantic_type = shape_type;
            hint.confidence    = std::min(0.95, shape_conf + 0.15);
            hint.rationale     = "label and data agree: " + shape_why;
        } else if (label_type != semantic_type_t::unknown) {
            // The label is more specific than the shape (quantity vs monetary,
            // category vs status). Trust it, but not blindly.
            hint.semantic_type = label_type;
            hint.confidence    = 0.65;
            hint.rationale     = "taken from the header label, data is " +
                                 std::string(to_string(stats.inferred_type));
        } else {
            hint.semantic_type = shape_type;
            hint.confidence    = shape_conf;
            hint.rationale     = shape_why;
        }

        if (hint.confidence < k_min_semantic_confidence) {
            hint.semantic_type = semantic_type_t::unknown;
            if (hint.rationale.empty()) hint.rationale = "not enough evidence";
        }

        hint.semantic_name = !label.empty() ? label
                                            : default_name(hint.semantic_type, stats.physical_index);

        // Names have to stay unique, otherwise a query cannot address a column.
        const int uses = name_uses[hint.semantic_name]++;
        if (uses > 0) hint.semantic_name += "_" + std::to_string(uses + 1);

        result.hints.push_back(std::move(hint));
    }

    return result;
}

merope::schema_t merope::build_schema(const dataset_profile_t& profile, const heuristic_result_t& hints) {
    schema_t schema;
    schema.dataset_path = profile.dataset_path;
    schema.dialect      = profile.dialect;
    schema.columns.reserve(profile.columns.size());

    for (const column_profile_t& stats : profile.columns) {
        column_schema_t column;
        column.physical_name  = stats.physical_name;
        column.physical_index = stats.physical_index;
        column.physical_type  = stats.inferred_type;
        schema.columns.push_back(std::move(column));
    }

    apply_hints(schema, hints);
    return schema;
}

void merope::apply_hints(schema_t& schema, const heuristic_result_t& hints, bool overwrite_confirmed) {
    for (const inference_hint_t& hint : hints.hints) {
        if (hint.physical_index >= schema.columns.size()) continue;
        if (!hint.proposed) continue;  // nobody filled this slot in; keep what is there
        column_schema_t& column = schema.columns[hint.physical_index];
        if (column.user_confirmed && !overwrite_confirmed) continue;

        column.semantic_name = hint.semantic_name;
        column.semantic_type = hint.semantic_type;
        column.confidence    = hint.confidence;
    }
}

std::string merope::format_hints(const heuristic_result_t& hints) {
    std::ostringstream out;
    char buffer[16];
    for (const inference_hint_t& hint : hints.hints) {
        std::snprintf(buffer, sizeof(buffer), "%.2f", hint.confidence);
        out << "  [" << hint.physical_index << "] " << hint.semantic_name << " -> "
            << to_string(hint.semantic_type) << " (" << buffer << ") " << hint.rationale << "\n";
    }
    return out.str();
}

