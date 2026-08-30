#include "schema.h"

#include "../core/json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

static bool equals_ignore_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

std::size_t merope::schema_t::index_of(std::string_view name) const noexcept {
    // Semantic names first: that is what a natural language query will use.
    std::size_t match = k_invalid_column;
    std::size_t hits  = 0;
    for (const column_schema_t& column : columns) {
        if (!column.semantic_name.empty() && equals_ignore_case(column.semantic_name, name)) {
            match = column.physical_index;
            ++hits;
        }
    }
    if (hits == 1) return match;
    if (hits > 1) return k_invalid_column;  // ambiguous, refuse rather than guess

    for (const column_schema_t& column : columns) {
        if (equals_ignore_case(column.physical_name, name)) {
            match = column.physical_index;
            ++hits;
        }
    }
    return hits == 1 ? match : k_invalid_column;
}

const merope::column_schema_t* merope::schema_t::find(std::string_view name) const noexcept {
    const std::size_t index = index_of(name);
    if (index == k_invalid_column) return nullptr;
    for (const column_schema_t& column : columns) {
        if (column.physical_index == index) return &column;
    }
    return nullptr;
}

merope::parse_options_t merope::schema_t::parse_options() const noexcept {
    parse_options_t options;
    options.decimal_separator   = dialect.decimal_separator;
    options.thousands_separator = dialect.thousands_separator;
    return options;
}

std::vector<std::size_t> merope::schema_t::resolve_all(const std::vector<std::string>& names,
                                                        std::vector<std::string>& unresolved) const {
    std::vector<std::size_t> indices;
    indices.reserve(names.size());
    for (const std::string& name : names) {
        const std::size_t index = index_of(name);
        if (index == k_invalid_column) {
            unresolved.push_back(name);
        } else {
            indices.push_back(index);
        }
    }
    return indices;
}

bool merope::schema_t::all_confirmed() const noexcept {
    return std::all_of(columns.begin(), columns.end(),
                       [](const column_schema_t& column) { return column.user_confirmed; });
}

std::string merope::schema_sidecar_path(const std::string& dataset_path) {
    return dataset_path + ".merope-schema.json";
}

bool merope::save_schema(const schema_t& schema, const std::string& path, std::string& error) {
    json_value_t root = json_value_t::make_object();
    root.set("dataset", json_value_t::make_string(schema.dataset_path));

    json_value_t dialect = json_value_t::make_object();
    dialect.set("delimiter", json_value_t::make_string(std::string(1, schema.dialect.delimiter)));
    dialect.set("quote", json_value_t::make_string(std::string(1, schema.dialect.quote)));
    dialect.set("has_header", json_value_t::make_bool(schema.dialect.has_header));
    dialect.set("encoding", json_value_t::make_string(to_string(schema.dialect.encoding)));
    dialect.set("decimal_separator",
                json_value_t::make_string(std::string(1, schema.dialect.decimal_separator)));
    dialect.set("thousands_separator",
                schema.dialect.thousands_separator == '\0'
                    ? json_value_t::make_null()
                    : json_value_t::make_string(std::string(1, schema.dialect.thousands_separator)));
    dialect.set("quoted_newlines", json_value_t::make_bool(schema.dialect.quoted_newlines));
    dialect.set("column_count", json_value_t::make_number(static_cast<double>(schema.dialect.column_count)));
    root.set("dialect", std::move(dialect));

    json_value_t columns = json_value_t::make_array();
    for (const column_schema_t& column : schema.columns) {
        json_value_t entry = json_value_t::make_object();
        entry.set("physical_name", json_value_t::make_string(column.physical_name));
        entry.set("physical_index", json_value_t::make_number(static_cast<double>(column.physical_index)));
        entry.set("physical_type", json_value_t::make_string(to_string(column.physical_type)));
        entry.set("semantic_name", json_value_t::make_string(column.semantic_name));
        entry.set("semantic_type", json_value_t::make_string(to_string(column.semantic_type)));
        entry.set("confidence", json_value_t::make_number(column.confidence));
        entry.set("user_confirmed", json_value_t::make_bool(column.user_confirmed));
        columns.array_value.push_back(std::move(entry));
    }
    root.set("columns", std::move(columns));

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot write schema to " + path;
        return false;
    }
    stream << json_serialize(root, 2) << "\n";
    return stream.good();
}

bool merope::load_schema(const std::string& path, schema_t& out, std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "no cached schema at " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();

    json_value_t root;
    if (!json_parse(buffer.str(), root, error)) return false;
    if (!root.is_object()) {
        error = "schema file is not a JSON object";
        return false;
    }

    out = schema_t{};
    out.dataset_path = root.string_or("dataset", "");

    if (const json_value_t* dialect = root.find("dialect"); dialect != nullptr) {
        const std::string delimiter = dialect->string_or("delimiter", ",");
        const std::string quote     = dialect->string_or("quote", "\"");
        const std::string decimal   = dialect->string_or("decimal_separator", ".");
        out.dialect.delimiter         = delimiter.empty() ? ',' : delimiter[0];
        out.dialect.quote             = quote.empty() ? '"' : quote[0];
        out.dialect.decimal_separator = decimal.empty() ? '.' : decimal[0];
        out.dialect.has_header        = dialect->bool_or("has_header", false);
        out.dialect.quoted_newlines   = dialect->bool_or("quoted_newlines", false);
        out.dialect.column_count      = static_cast<std::size_t>(dialect->int_or("column_count", 0));
        if (encoding_t encoding = encoding_t::utf8;
            encoding_from_string(dialect->string_or("encoding", ""), encoding)) {
            out.dialect.encoding = encoding;
        }
        const json_value_t* thousands = dialect->find("thousands_separator");
        out.dialect.thousands_separator =
            thousands != nullptr && thousands->is_string() && !thousands->string_value.empty()
                ? thousands->string_value[0]
                : '\0';
    }

    const json_value_t* columns = root.find("columns");
    if (columns == nullptr || !columns->is_array()) {
        error = "schema file has no columns array";
        return false;
    }
    for (const json_value_t& entry : columns->array_value) {
        column_schema_t column;
        column.physical_name  = entry.string_or("physical_name", "");
        column.physical_index = static_cast<std::size_t>(entry.int_or("physical_index", 0));
        column.physical_type  = data_type_from_string(entry.string_or("physical_type", "UNKNOWN"));
        column.semantic_name  = entry.string_or("semantic_name", "");
        column.semantic_type  = semantic_type_from_string(entry.string_or("semantic_type", "UNKNOWN"));
        column.confidence     = entry.number_or("confidence", 0.0);
        column.user_confirmed = entry.bool_or("user_confirmed", false);
        out.columns.push_back(std::move(column));
    }
    if (out.dialect.column_count == 0) out.dialect.column_count = out.columns.size();

    out.dialect.column_names.clear();
    for (const column_schema_t& column : out.columns) {
        out.dialect.column_names.push_back(column.physical_name);
    }
    return true;
}

std::string merope::format_schema_table(const schema_t& schema) {
    std::size_t physical_width = 13;
    std::size_t semantic_width = 13;
    for (const column_schema_t& column : schema.columns) {
        physical_width = std::max(physical_width, column.physical_name.size());
        semantic_width = std::max(semantic_width, column.semantic_name.size());
    }

    std::ostringstream out;
    auto pad = [](const std::string& text, std::size_t width) {
        std::string padded = text;
        if (padded.size() < width) padded.append(width - padded.size(), ' ');
        return padded;
    };

    out << "  " << pad("#", 4) << pad("physical_name", physical_width + 2)
        << pad("physical_type", 14) << pad("semantic_name", semantic_width + 2)
        << pad("semantic_type", 14) << pad("conf", 7) << "confirmed\n";
    out << "  " << std::string(4 + physical_width + 2 + 14 + semantic_width + 2 + 14 + 7 + 9, '-') << "\n";

    for (const column_schema_t& column : schema.columns) {
        char confidence[8];
        std::snprintf(confidence, sizeof(confidence), "%.2f", column.confidence);
        out << "  " << pad(std::to_string(column.physical_index), 4)
            << pad(column.physical_name, physical_width + 2)
            << pad(to_string(column.physical_type), 14)
            << pad(column.semantic_name.empty() ? "-" : column.semantic_name, semantic_width + 2)
            << pad(to_string(column.semantic_type), 14)
            << pad(confidence, 7)
            << (column.user_confirmed ? "yes" : "no") << "\n";
    }
    return out.str();
}

