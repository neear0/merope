#include "api.h"

#include "../app/pipeline.h"
#include "../core/json.h"
#include "../dataset/generator.h"
#include "../plan/plan_validator.h"
#include "ui_assets.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>

// A JSON number reaches us as std::int64_t and every field below is a
// std::size_t bound on how much work or memory a request may cost. Casting a
// negative value straight across wraps it to a colossal positive one, which
// removes the bound rather than tightening it: chunk_rows -1 becomes SIZE_MAX
// and the reader buffers a whole partition. Clamp before the cast, never after.
static std::size_t clamped_size(std::int64_t value, std::int64_t low, std::int64_t high) noexcept {
    return static_cast<std::size_t>(std::clamp(value, low, high));
}

static merope::json_value_t error_object(const std::string& message) {
    merope::json_value_t root = merope::json_value_t::make_object();
    root.set("ok", merope::json_value_t::make_bool(false));
    root.set("error", merope::json_value_t::make_string(message));
    return root;
}

static void reply(merope::http_response_t& response, const merope::json_value_t& payload, int status = 200) {
    response.status = status;
    response.json(merope::json_serialize(payload, 0));
}

static void reply_error(merope::http_response_t& response, const std::string& message, int status = 400) {
    reply(response, error_object(message), status);
}

static bool read_json_body(const merope::http_request_t& request, merope::json_value_t& out, std::string& error) {
    if (request.body.empty()) {
        out = merope::json_value_t::make_object();
        return true;
    }
    return merope::json_parse(request.body, out, error);
}

// Only requests that really came from this machine are served. Combined with
// the loopback bind, this blunts DNS rebinding, where a page on some other site
// resolves a hostname to 127.0.0.1 and then talks to this API.
static bool host_is_local(const merope::http_request_t& request) {
    std::string host = request.header("Host");
    const std::size_t colon = host.rfind(':');
    if (colon != std::string::npos) host.resize(colon);
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return host.empty() || host == "localhost" || host == "127.0.0.1" || host == "[::1]" || host == "::1";
}

std::string merope::make_session_token() {
    // Not a secret worth protecting for long: it exists so that a page on some
    // other site, which cannot read anything this origin returns, cannot fire
    // the one endpoint whose effect needs no reply to be useful.
    std::random_device source;
    static const char  k_hex[] = "0123456789abcdef";

    std::string token;
    token.reserve(32);
    for (int word = 0; word < 4; ++word) {
        const std::uint32_t value = static_cast<std::uint32_t>(source());
        for (int nibble = 7; nibble >= 0; --nibble) {
            token.push_back(k_hex[(value >> (nibble * 4)) & 0xFu]);
        }
    }
    return token;
}

bool merope::shutdown_token_matches(const std::string& expected, const std::string& presented) {
    // No early exit on the first differing byte: a wrong token should take the
    // same time as a right one.
    if (expected.empty() || presented.size() != expected.size()) return false;
    unsigned int difference = 0;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        difference |= static_cast<unsigned int>(static_cast<unsigned char>(expected[index]) ^
                                                static_cast<unsigned char>(presented[index]));
    }
    return difference == 0;
}

merope::ai_settings_t merope::settings_for_request(const web_options_t& options,
                                                   const std::string& model) {
    ai_settings_t settings = options.ai.settings;
    // The page may name any model the provider serves, but it can never point
    // the process at a different provider, a different endpoint, or a different
    // key: those are decided by whoever started the server.
    if (!model.empty() && options.ai.remote) settings.model = model;
    return settings;
}

static bool resolve_dataset(const merope::web_options_t& options, const std::string& requested,
                            std::string& resolved, std::string& error) {
    return merope::resolve_dataset_path(options.data_root, requested, resolved, error);
}

static std::string relative_to_root(const merope::web_options_t& options, const std::string& absolute) {
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::weakly_canonical(options.data_root, ec);
    const std::filesystem::path path = std::filesystem::weakly_canonical(absolute, ec);
    if (ec) return absolute;
    const std::filesystem::path relative = std::filesystem::relative(path, root, ec);
    return ec ? absolute : relative.generic_string();
}

static merope::json_value_t cell_to_json_number(const merope::cell_value_t& value, merope::data_type_t type) {
    if (merope::is_null(value)) return merope::json_value_t::make_null();
    if (const auto* real = std::get_if<double>(&value)) {
        return merope::json_value_t::make_number(*real);
    }
    if (const auto* integral = std::get_if<std::int64_t>(&value)) {
        if (type == merope::data_type_t::decimal) {
            return merope::json_value_t::make_number(static_cast<double>(*integral) /
                                             static_cast<double>(merope::k_money_factor));
        }
        if (type == merope::data_type_t::int64) return merope::json_value_t::make_number(static_cast<double>(*integral));
    }
    return merope::json_value_t::make_null();
}

static merope::json_value_t string_array(const std::vector<std::string>& values) {
    merope::json_value_t out = merope::json_value_t::make_array();
    for (const std::string& value : values) {
        out.array_value.push_back(merope::json_value_t::make_string(value));
    }
    return out;
}

static merope::json_value_t dialect_to_json(const merope::csv_dialect_t& dialect) {
    merope::json_value_t out = merope::json_value_t::make_object();
    out.set("delimiter", merope::json_value_t::make_string(dialect.delimiter == '\t'
                                                       ? std::string("\\t")
                                                       : std::string(1, dialect.delimiter)));
    out.set("encoding", merope::json_value_t::make_string(merope::to_string(dialect.encoding)));
    out.set("has_header", merope::json_value_t::make_bool(dialect.has_header));
    out.set("header_confidence", merope::json_value_t::make_number(dialect.header_confidence));
    out.set("delimiter_confidence", merope::json_value_t::make_number(dialect.delimiter_confidence));
    out.set("decimal_separator", merope::json_value_t::make_string(std::string(1, dialect.decimal_separator)));
    out.set("thousands_separator",
            dialect.thousands_separator == '\0'
                ? merope::json_value_t::make_string("none")
                : merope::json_value_t::make_string(std::string(1, dialect.thousands_separator)));
    out.set("quoted_newlines", merope::json_value_t::make_bool(dialect.quoted_newlines));
    out.set("column_count", merope::json_value_t::make_number(static_cast<double>(dialect.column_count)));
    return out;
}

static merope::json_value_t report_to_json(const merope::execution_report_t& report) {
    merope::json_value_t out = merope::json_value_t::make_object();
    out.set("dataset_size_bytes", merope::json_value_t::make_number(static_cast<double>(report.dataset_size_bytes)));
    out.set("dataset_size", merope::json_value_t::make_string(merope::format_bytes(report.dataset_size_bytes)));
    out.set("records", merope::json_value_t::make_number(static_cast<double>(report.records_processed)));
    out.set("rows_after_filter", merope::json_value_t::make_number(static_cast<double>(report.rows_after_filter)));
    out.set("bad_rows", merope::json_value_t::make_number(static_cast<double>(report.bad_rows)));
    out.set("quarantined", merope::json_value_t::make_number(static_cast<double>(report.quarantined_rows)));
    out.set("bad_row_policy", merope::json_value_t::make_string(report.bad_row_policy));
    out.set("workers", merope::json_value_t::make_number(static_cast<double>(report.workers)));
    out.set("partitions", merope::json_value_t::make_number(static_cast<double>(report.partitions)));
    out.set("partition_note", merope::json_value_t::make_string(report.partition_note));
    out.set("seconds", merope::json_value_t::make_number(report.processing_seconds));
    out.set("throughput_mb_s", merope::json_value_t::make_number(report.throughput_mb_per_second()));
    out.set("merope::peak_memory_bytes", merope::json_value_t::make_number(static_cast<double>(report.peak_memory)));
    out.set("peak_memory", merope::json_value_t::make_string(merope::format_bytes(report.peak_memory)));
    out.set("groups", merope::json_value_t::make_number(static_cast<double>(report.groups)));
    out.set("has_baseline", merope::json_value_t::make_bool(report.has_baseline));
    if (report.has_baseline) {
        out.set("baseline_seconds", merope::json_value_t::make_number(report.baseline_seconds));
        out.set("speedup", merope::json_value_t::make_number(report.speedup()));
        out.set("efficiency", merope::json_value_t::make_number(report.efficiency()));
    }
    return out;
}

// One entry per column: the physical facts, the semantic proposal, and the
// profile evidence behind it, so the review screen can show why.
static merope::json_value_t columns_to_json(const merope::schema_t& schema, const merope::dataset_profile_t& profile,
                                             const merope::heuristic_result_t& hints, bool have_profile) {
    merope::json_value_t columns = merope::json_value_t::make_array();

    for (const merope::column_schema_t& column : schema.columns) {
        merope::json_value_t entry = merope::json_value_t::make_object();
        entry.set("physical_index", merope::json_value_t::make_number(static_cast<double>(column.physical_index)));
        entry.set("physical_name", merope::json_value_t::make_string(column.physical_name));
        entry.set("physical_type", merope::json_value_t::make_string(merope::to_string(column.physical_type)));
        entry.set("semantic_name", merope::json_value_t::make_string(column.semantic_name));
        entry.set("semantic_type", merope::json_value_t::make_string(merope::to_string(column.semantic_type)));
        entry.set("confidence", merope::json_value_t::make_number(column.confidence));
        entry.set("user_confirmed", merope::json_value_t::make_bool(column.user_confirmed));

        std::string rationale;
        for (const merope::inference_hint_t& hint : hints.hints) {
            if (hint.physical_index == column.physical_index) rationale = hint.rationale;
        }
        entry.set("rationale", merope::json_value_t::make_string(rationale));

        if (have_profile && column.physical_index < profile.columns.size()) {
            const merope::column_profile_t& stats = profile.columns[column.physical_index];
            merope::json_value_t            statistics = merope::json_value_t::make_object();
            statistics.set("row_count", merope::json_value_t::make_number(static_cast<double>(stats.row_count)));
            statistics.set("null_count", merope::json_value_t::make_number(static_cast<double>(stats.null_count)));
            statistics.set("unique_count", merope::json_value_t::make_number(static_cast<double>(stats.unique_count)));
            statistics.set("unique_ratio", merope::json_value_t::make_number(stats.unique_ratio));
            statistics.set("unique_exact", merope::json_value_t::make_bool(stats.unique_exact));
            if (stats.has_min) {
                statistics.set("min", merope::json_value_t::make_string(
                                          merope::cell_to_display(stats.min_value, stats.inferred_type)));
            }
            if (stats.has_max) {
                statistics.set("max", merope::json_value_t::make_string(
                                          merope::cell_to_display(stats.max_value, stats.inferred_type)));
            }
            if (stats.has_mean) statistics.set("mean", merope::json_value_t::make_number(stats.mean));
            if (stats.date_pattern != merope::date_pattern_t::none) {
                statistics.set("date_pattern", merope::json_value_t::make_string(merope::to_string(stats.date_pattern)));
            }
            statistics.set("examples", string_array(stats.examples));
            entry.set("stats", std::move(statistics));
        }

        columns.array_value.push_back(std::move(entry));
    }
    return columns;
}

// Loads the schema the way the query screen needs it: confirmed if the user has
// confirmed one, freshly inferred otherwise.
static bool load_working_schema(const std::string& path, const merope::web_options_t& options, merope::schema_t& out,
                                bool& confirmed, std::string& error) {
    // The configured provider, not the mock: if a model named these columns
    // the first time, the same one should be answering about them now.
    std::unique_ptr<merope::c_ai_provider> provider =
        merope::make_ai_provider(merope::settings_for_request(options, std::string()));

    merope::sample_options_t sample;
    sample.target_rows = options.sample_rows;
    sample.seed        = options.seed;

    try {
        merope::inspection_t inspection = inspect_dataset(path, sample, provider.get(), true);
        out       = std::move(inspection.schema);
        confirmed = inspection.from_cache;
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

bool merope::resolve_dataset_path(const std::string& root_path, const std::string& requested,
                                   std::string& resolved, std::string& error) {
    if (requested.empty()) {
        error = "no dataset path given";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path root = std::filesystem::weakly_canonical(root_path, ec);
    if (ec) {
        error = "the data directory does not exist: " + root_path;
        return false;
    }

    std::filesystem::path candidate(requested);
    if (candidate.is_relative()) candidate = root / candidate;
    candidate = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
        error = "no such dataset: " + requested;
        return false;
    }

    // Compare canonical paths component by component. A string prefix test
    // would accept "…/data_other" as being inside "…/data", and normalising
    // first is what stops ".." from walking out.
    auto root_it      = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *candidate_it != *root_it) {
            error = "dataset is outside the served directory";
            return false;
        }
    }

    if (!std::filesystem::exists(candidate, ec) || std::filesystem::is_directory(candidate, ec)) {
        error = "no such dataset: " + requested;
        return false;
    }
    resolved = candidate.string();
    return true;
}

void merope::register_api(c_http_server& server, const web_options_t& options) {
    const web_options_t settings = options;

    auto guarded = [](http_handler_t handler) {
        return [handler](const http_request_t& request, http_response_t& response) {
            if (!host_is_local(request)) {
                reply_error(response, "this API only answers requests from this machine", 403);
                return;
            }
            try {
                handler(request, response);
            } catch (const std::exception& failure) {
                reply_error(response, failure.what(), 500);
            }
        };
    };

    // ---- the single page application itself -------------------------------
    auto serve_ui = [](const http_request_t&, http_response_t& response) {
        response.status       = 200;
        response.content_type = "text/html; charset=utf-8";
        response.body         = ui_document();
        response.headers.emplace_back("Cache-Control", "no-store");
    };
    server.route("GET", "/", serve_ui);
    server.route("GET", "/index.html", serve_ui);

    // ---- which datasets are available ------------------------------------
    server.route("GET", "/api/datasets", guarded([settings](const http_request_t&,
                                                            http_response_t& response) {
        json_value_t root = json_value_t::make_object();
        root.set("ok", json_value_t::make_bool(true));
        root.set("root", json_value_t::make_string(settings.data_root));

        json_value_t files = json_value_t::make_array();
        std::error_code ec;
        const std::filesystem::path base(settings.data_root);
        if (std::filesystem::exists(base, ec)) {
            std::vector<std::filesystem::directory_entry> entries;
            for (std::filesystem::recursive_directory_iterator it(
                     base, std::filesystem::directory_options::skip_permission_denied, ec);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
                if (!it->is_regular_file(ec)) continue;
                std::string extension = it->path().extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (extension != ".csv" && extension != ".tsv" && extension != ".txt") continue;
                entries.push_back(*it);
            }
            std::sort(entries.begin(), entries.end(),
                      [](const std::filesystem::directory_entry& left,
                         const std::filesystem::directory_entry& right) {
                          return left.path() < right.path();
                      });
            for (const std::filesystem::directory_entry& entry : entries) {
                json_value_t item = json_value_t::make_object();
                const std::uintmax_t size = entry.file_size(ec);
                item.set("path", json_value_t::make_string(
                                     relative_to_root(settings, entry.path().string())));
                item.set("size_bytes", json_value_t::make_number(static_cast<double>(ec ? 0 : size)));
                item.set("size", json_value_t::make_string(format_bytes(ec ? 0 : size)));
                const std::string sidecar = schema_sidecar_path(entry.path().string());
                item.set("has_schema", json_value_t::make_bool(std::filesystem::exists(sidecar, ec)));
                files.array_value.push_back(std::move(item));
            }
        }
        root.set("datasets", std::move(files));
        reply(response, root);
    }));

    // ---- sniff, sample, profile, infer -----------------------------------
    server.route("POST", "/api/inspect", guarded([settings](const http_request_t& request,
                                                            http_response_t& response) {
        json_value_t body;
        std::string  error;
        if (!read_json_body(request, body, error)) {
            reply_error(response, "malformed request: " + error);
            return;
        }

        std::string path;
        if (!resolve_dataset(settings, body.string_or("path", ""), path, error)) {
            reply_error(response, error);
            return;
        }

        sample_options_t sample;
        // The reservoir is sized from this, so a negative value would ask it to
        // hold the entire file rather than a sample of it.
        sample.target_rows = clamped_size(
            body.int_or("sample_rows", static_cast<std::int64_t>(settings.sample_rows)),
            1, 10000000);
        sample.seed = static_cast<std::uint64_t>(
            body.int_or("seed", static_cast<std::int64_t>(settings.seed)));

        std::unique_ptr<c_ai_provider> provider;
        if (!body.bool_or("no_ai", false)) {
            provider = make_ai_provider(settings_for_request(settings, body.string_or("model", "")));
        }

        // Always profile, so the review screen has evidence to show, then lay
        // any confirmed decisions on top of it.
        inspection_t inspection = inspect_dataset(path, sample, provider.get(), false);
        const std::string cache = schema_sidecar_path(path);
        const bool        confirmed = overlay_confirmed_schema(inspection.schema, cache);

        json_value_t root = json_value_t::make_object();
        root.set("ok", json_value_t::make_bool(true));
        root.set("path", json_value_t::make_string(relative_to_root(settings, path)));
        root.set("absolute_path", json_value_t::make_string(path));
        root.set("from_cache", json_value_t::make_bool(confirmed));
        root.set("file_size_bytes", json_value_t::make_number(
                                        static_cast<double>(inspection.sniff.file_size_bytes)));
        root.set("file_size", json_value_t::make_string(format_bytes(inspection.sniff.file_size_bytes)));
        root.set("dialect", dialect_to_json(inspection.schema.dialect));
        root.set("format_notes", string_array(inspection.sniff.notes));

        json_value_t sampling = json_value_t::make_object();
        sampling.set("method", json_value_t::make_string(to_string(inspection.profile.sample_method)));
        sampling.set("rows", json_value_t::make_number(
                                 static_cast<double>(inspection.profile.sample_rows)));
        sampling.set("rows_scanned", json_value_t::make_number(
                                         static_cast<double>(inspection.profile.rows_scanned)));
        sampling.set("seed", json_value_t::make_number(static_cast<double>(inspection.profile.seed)));
        sampling.set("exact", json_value_t::make_bool(inspection.profile.exact));
        sampling.set("estimated_total_rows",
                     json_value_t::make_number(
                         static_cast<double>(inspection.profile.estimated_total_rows)));
        root.set("sampling", std::move(sampling));

        root.set("columns", columns_to_json(inspection.schema, inspection.profile,
                                            inspection.ai_ran ? inspection.ai.proposals
                                                              : inspection.heuristics,
                                            true));

        json_value_t ai = json_value_t::make_object();
        ai.set("ran", json_value_t::make_bool(inspection.ai_ran));
        ai.set("provider", json_value_t::make_string(inspection.ai.provider));
        ai.set("notes", string_array(inspection.ai.notes));
        ai.set("prompt", json_value_t::make_string(build_schema_prompt(inspection.profile)));
        root.set("ai", std::move(ai));

        reply(response, root);
    }));

    // ---- the user accepts or edits the schema ----------------------------
    server.route("POST", "/api/confirm", guarded([settings](const http_request_t& request,
                                                            http_response_t& response) {
        json_value_t body;
        std::string  error;
        if (!read_json_body(request, body, error)) {
            reply_error(response, "malformed request: " + error);
            return;
        }

        std::string path;
        if (!resolve_dataset(settings, body.string_or("path", ""), path, error)) {
            reply_error(response, error);
            return;
        }

        sample_options_t sample;
        sample.target_rows = settings.sample_rows;
        sample.seed        = settings.seed;

        std::unique_ptr<c_ai_provider> provider =
            make_ai_provider(settings_for_request(settings, body.string_or("model", "")));
        inspection_t inspection = inspect_dataset(path, sample, provider.get(), false);
        overlay_confirmed_schema(inspection.schema, schema_sidecar_path(path));

        // Apply whatever the user edited before confirming.
        if (const json_value_t* edits = body.find("columns"); edits != nullptr && edits->is_array()) {
            for (const json_value_t& edit : edits->array_value) {
                const std::size_t index = static_cast<std::size_t>(edit.int_or("physical_index", -1));
                if (index >= inspection.schema.columns.size()) continue;
                column_schema_t& column = inspection.schema.columns[index];
                if (const json_value_t* name = edit.find("semantic_name");
                    name != nullptr && name->is_string()) {
                    column.semantic_name = name->string_value;
                }
                if (const json_value_t* type = edit.find("semantic_type");
                    type != nullptr && type->is_string()) {
                    column.semantic_type = semantic_type_from_string(type->string_value);
                }
            }
        }

        if (!confirm_and_save(inspection.schema, error)) {
            reply_error(response, error, 500);
            return;
        }

        json_value_t root = json_value_t::make_object();
        root.set("ok", json_value_t::make_bool(true));
        root.set("saved_to", json_value_t::make_string(schema_sidecar_path(path)));
        root.set("columns", columns_to_json(inspection.schema, inspection.profile,
                                            inspection.heuristics, true));
        reply(response, root);
    }));

    // ---- ask a question, or run a plan directly --------------------------
    auto run_and_reply = [](const schema_t& schema, query_outcome_t& outcome, bool confirmed,
                            http_response_t& response) {
        json_value_t root = json_value_t::make_object();
        root.set("ok", json_value_t::make_bool(true));
        root.set("schema_confirmed", json_value_t::make_bool(confirmed));
        root.set("accepted", json_value_t::make_bool(outcome.accepted));
        root.set("logical_plan", plan_to_json(outcome.logical));
        root.set("plan_text", json_value_t::make_string(format_plan(outcome.logical)));
        root.set("warnings", string_array(outcome.validation.warnings));
        root.set("errors", string_array(outcome.validation.errors));

        if (!outcome.accepted) {
            reply(response, root);
            return;
        }

        root.set("physical_plan",
                 json_value_t::make_string(format_physical_plan(outcome.validation.plan, schema)));

        json_value_t types = json_value_t::make_array();
        for (const data_type_t type : outcome.result.types) {
            types.array_value.push_back(json_value_t::make_string(to_string(type)));
        }
        root.set("columns", string_array(outcome.result.columns));
        root.set("types", std::move(types));

        json_value_t rows    = json_value_t::make_array();
        json_value_t numeric = json_value_t::make_array();
        for (const std::vector<cell_value_t>& row : outcome.result.rows) {
            json_value_t display = json_value_t::make_array();
            json_value_t numbers = json_value_t::make_array();
            for (std::size_t index = 0; index < row.size(); ++index) {
                const data_type_t type = index < outcome.result.types.size()
                                             ? outcome.result.types[index]
                                             : data_type_t::utf8;
                display.array_value.push_back(json_value_t::make_string(cell_to_display(row[index], type)));
                numbers.array_value.push_back(cell_to_json_number(row[index], type));
            }
            rows.array_value.push_back(std::move(display));
            numeric.array_value.push_back(std::move(numbers));
        }
        root.set("rows", std::move(rows));
        root.set("numeric", std::move(numeric));
        root.set("limited", json_value_t::make_bool(outcome.result.limited));
        root.set("result_warning", json_value_t::make_string(outcome.result.warning));
        root.set("report", report_to_json(outcome.report));
        reply(response, root);
    };

    server.route("POST", "/api/query", guarded([settings, run_and_reply](
                                                   const http_request_t& request,
                                                   http_response_t& response) {
        json_value_t body;
        std::string  error;
        if (!read_json_body(request, body, error)) {
            reply_error(response, "malformed request: " + error);
            return;
        }

        std::string path;
        if (!resolve_dataset(settings, body.string_or("path", ""), path, error)) {
            reply_error(response, error);
            return;
        }
        const std::string question = body.string_or("question", "");
        if (question.empty()) {
            reply_error(response, "no question given");
            return;
        }

        schema_t schema;
        bool     confirmed = false;
        if (!load_working_schema(path, settings, schema, confirmed, error)) {
            reply_error(response, error);
            return;
        }

        execution_options_t execution;
        // 0 means "decide for me" for workers and partitions, so the floor is 0
        // there and 1 for chunk_rows, which the reader would otherwise default.
        execution.workers    = clamped_size(body.int_or("workers", 0), 0, 4096);
        execution.partitions = clamped_size(body.int_or("partitions", 0), 0, 65536);
        execution.chunk_rows =
            clamped_size(body.int_or("chunk_rows", k_default_chunk_rows), 1, 10000000);
        execution.policy = bad_row_policy_from_string(body.string_or("policy", "skip"));
        execution.measure_baseline = body.bool_or("baseline", false);
        if (const std::int64_t minimum = body.int_or("min_partition_bytes", 0); minimum > 0) {
            execution.min_partition_bytes = static_cast<std::uint64_t>(minimum);
        }

        std::unique_ptr<c_ai_provider> provider =
            make_ai_provider(settings_for_request(settings, body.string_or("model", "")));
        query_outcome_t outcome = run_query(schema, *provider, question, execution);
        run_and_reply(schema, outcome, confirmed, response);
    }));

    server.route("POST", "/api/plan", guarded([settings, run_and_reply](
                                                  const http_request_t& request,
                                                  http_response_t& response) {
        json_value_t body;
        std::string  error;
        if (!read_json_body(request, body, error)) {
            reply_error(response, "malformed request: " + error);
            return;
        }

        std::string path;
        if (!resolve_dataset(settings, body.string_or("path", ""), path, error)) {
            reply_error(response, error);
            return;
        }

        const json_value_t* plan = body.find("plan");
        if (plan == nullptr) {
            reply_error(response, "no plan given");
            return;
        }
        const std::string plan_json =
            plan->is_string() ? plan->string_value : json_serialize(*plan, 0);

        schema_t schema;
        bool     confirmed = false;
        if (!load_working_schema(path, settings, schema, confirmed, error)) {
            reply_error(response, error);
            return;
        }

        execution_options_t execution;
        execution.workers    = clamped_size(body.int_or("workers", 0), 0, 4096);
        execution.partitions = clamped_size(body.int_or("partitions", 0), 0, 65536);
        execution.policy     = bad_row_policy_from_string(body.string_or("policy", "skip"));
        execution.measure_baseline = body.bool_or("baseline", false);

        std::string     parse_error;
        query_outcome_t outcome = run_plan_json(schema, plan_json, execution, parse_error);
        if (!parse_error.empty()) {
            reply_error(response, "the plan is not valid: " + parse_error);
            return;
        }
        run_and_reply(schema, outcome, confirmed, response);
    }));

    // ---- synthetic data, so the UI is usable on a fresh checkout ---------
    if (settings.allow_generate) {
        server.route("POST", "/api/generate", guarded([settings](const http_request_t& request,
                                                                 http_response_t& response) {
            json_value_t body;
            std::string  error;
            if (!read_json_body(request, body, error)) {
                reply_error(response, "malformed request: " + error);
                return;
            }

            // The output name is confined to the data root and stripped of any
            // directory component, so this cannot write outside it.
            std::filesystem::path requested(body.string_or("path", "generated.csv"));
            std::string           name = requested.filename().string();
            if (name.empty() || name == "." || name == "..") name = "generated.csv";
            if (std::filesystem::path(name).extension().empty()) name += ".csv";

            std::error_code ec;
            const std::filesystem::path root = std::filesystem::weakly_canonical(settings.data_root, ec);
            if (ec) {
                reply_error(response, "the data directory does not exist", 500);
                return;
            }
            std::filesystem::create_directories(root, ec);
            const std::filesystem::path target = root / name;

            generator_options_t generator;
            generator.rows = static_cast<std::uint64_t>(std::clamp<std::int64_t>(
                body.int_or("rows", 200000), 1, 50000000));
            generator.seed             = static_cast<std::uint64_t>(body.int_or("seed", 20260101));
            generator.write_header     = body.bool_or("header", true);
            generator.corrupt_fraction = std::clamp(body.number_or("corrupt", 0.0), 0.0, 0.5);

            const generator_stats_t stats = generate_dataset(target.string(), generator);

            json_value_t root_object = json_value_t::make_object();
            root_object.set("ok", json_value_t::make_bool(true));
            root_object.set("path", json_value_t::make_string(name));
            root_object.set("rows", json_value_t::make_number(static_cast<double>(stats.rows_written)));
            root_object.set("bytes", json_value_t::make_number(static_cast<double>(stats.bytes_written)));
            root_object.set("size", json_value_t::make_string(format_bytes(stats.bytes_written)));
            root_object.set("corrupted", json_value_t::make_number(
                                             static_cast<double>(stats.corrupted_rows)));
            // The generator knows the true answers; the UI shows them so a user
            // can check the engine rather than take its word for it.
            root_object.set("exact_total_amount",
                            json_value_t::make_string(cell_to_display(
                                cell_value_t{stats.total_amount_scaled}, data_type_t::decimal)));
            root_object.set("rows_in_2025",
                            json_value_t::make_number(static_cast<double>(stats.rows_in_2025)));
            reply(response, root_object);
        }));
    }

    // ---- what this process allows, and the token that proves same origin ---
    const std::string    session_token = make_session_token();
    c_http_server* const target        = &server;

    server.route("GET", "/api/session", guarded([settings, session_token, target](
                                                    const http_request_t&,
                                                    http_response_t& response) {
        json_value_t root = json_value_t::make_object();
        root.set("ok", json_value_t::make_bool(true));
        root.set("root", json_value_t::make_string(settings.data_root));
        root.set("port", json_value_t::make_number(static_cast<double>(target->port())));
        root.set("can_generate", json_value_t::make_bool(settings.allow_generate));
        root.set("can_shutdown", json_value_t::make_bool(settings.allow_shutdown));

        // Which model is answering, so the page can say so and offer the rest.
        // The key is not here, and there is no endpoint that returns it.
        json_value_t ai = json_value_t::make_object();
        ai.set("provider", json_value_t::make_string(settings.ai.settings.provider));
        ai.set("model", json_value_t::make_string(settings.ai.settings.model));
        ai.set("remote", json_value_t::make_bool(settings.ai.remote));
        ai.set("source", json_value_t::make_string(settings.ai.source));
        ai.set("note", json_value_t::make_string(settings.ai.note));
        root.set("ai", std::move(ai));
        // Only the page served from this origin can read this. Nothing here
        // sends an Access-Control-Allow-Origin, so a script on another site
        // gets the bytes refused by its own browser.
        root.set("token", json_value_t::make_string(session_token));
        response.headers.emplace_back("Cache-Control", "no-store");
        reply(response, root);
    }));

    // ---- which models this key may use ------------------------------------
    // Asked of the provider rather than answered from a list in the source: a
    // hardcoded list of model names is wrong within a month.
    server.route("GET", "/api/models", guarded([settings](const http_request_t&,
                                                          http_response_t& response) {
        json_value_t root = json_value_t::make_object();
        root.set("ok", json_value_t::make_bool(true));
        root.set("provider", json_value_t::make_string(settings.ai.settings.provider));
        root.set("current", json_value_t::make_string(settings.ai.settings.model));
        root.set("remote", json_value_t::make_bool(settings.ai.remote));

        std::vector<std::string> models;
        std::string              error;
        const bool               listed = list_models(settings.ai.settings, models, error);
        root.set("listed", json_value_t::make_bool(listed));
        if (!listed) root.set("error", json_value_t::make_string(error));
        root.set("models", string_array(models));
        reply(response, root);
    }));

    // ---- the kill button --------------------------------------------------
    // The only route that acts on the server rather than on a dataset, so it
    // carries a guard of its own on top of the loopback bind and the Host
    // check: without the session token, nothing stops.
    server.route("POST", "/api/shutdown", guarded([settings, session_token, target](
                                                      const http_request_t& request,
                                                      http_response_t& response) {
        if (!settings.allow_shutdown) {
            reply_error(response, "this server was started with --no-kill", 403);
            return;
        }
        if (!shutdown_token_matches(session_token, request.header("X-Merope-Token"))) {
            // A form on another site can post here without reading anything
            // back, which would be enough to stop the process. It cannot read
            // the token, and it cannot set a custom header without a preflight
            // this server never approves.
            reply_error(response, "the session token is missing or wrong", 403);
            return;
        }

        json_value_t root = json_value_t::make_object();
        root.set("ok", json_value_t::make_bool(true));
        root.set("stopped", json_value_t::make_bool(true));
        root.set("port", json_value_t::make_number(static_cast<double>(target->port())));
        root.set("message", json_value_t::make_string(
                                "the engine is stopping; this port will stop answering"));
        reply(response, root);

        // Closes the listener, nothing else. This reply is already written and
        // will be sent on a socket this thread owns, and run() waits for the
        // pool before it returns, so the browser is answered before the process
        // goes away.
        target->stop();
        std::cout << "\nstopped from the web UI\n" << std::flush;
    }));
}
