#include "app/cli.h"
#include "app/pipeline.h"
#include "bench/benchmark.h"
#include "dataset/generator.h"
#include "plan/plan_validator.h"
#include "web/api.h"
#include "web/http_server.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>


void merope::print_usage() {
    std::cout <<
        "merope - automatic schema inference and parallel processing of unknown datasets\n"
        "\n"
        "  merope profile <csv> [options]\n"
        "      Sniff the format, sample the file, profile every column and show the\n"
        "      inferred schema. Nothing is written.\n"
        "\n"
        "  merope schema <csv> [--confirm] [options]\n"
        "      Show the inferred schema. With --confirm, mark it confirmed and cache it\n"
        "      next to the dataset so the AI is not asked again on every query.\n"
        "\n"
        "  merope query <csv> \"<question>\" [options]\n"
        "      Translate the question into a plan, validate it, and run it.\n"
        "\n"
        "  merope plan <csv> <plan.json> [options]\n"
        "      Validate and run a plan supplied directly, bypassing the model.\n"
        "\n"
        "  merope gen <csv> [--rows N] [--no-header] [--corrupt F] [--seed S]\n"
        "      Write a synthetic dataset for the experiments.\n"
        "\n"
        "  merope serve [--port N] [--data DIR] [--no-open] [--no-kill]\n"
        "      Start the web UI on 127.0.0.1 and open it. Same pipeline as the CLI.\n"
        "\n"
        "  merope bench [--suite LIST] [--out DIR] [--sizes GB,..] [--threads N,..]\n"
        "      Run the experiments from chapter 12 and write the results as CSV.\n"
        "\n"
        "  merope models [--provider P]\n"
        "      Ask the configured provider which models this key can use.\n"
        "\n"
        "  merope selftest\n"
        "      Run the built in checks.\n"
        "\n"
        "Options\n"
        "  --suite LIST       inference,sampling,threads,scaling,baseline (default: all)\n"
        "  --out DIR          where benchmark results go (default: bench)\n"
        "  --sizes GB,..      dataset sizes for the scaling suite (default: 1,5,10)\n"
        "  --threads N,..     worker counts for the parallel suite (default: 1,2,4,8,16,32)\n"
        "  --repeats N        timed runs per point, median reported (default: 3)\n"
        "  --keep             keep the datasets the benchmark generates\n"
        "  --port N           web UI port (default: 7433, 0 picks a free one)\n"
        "  --data DIR         directory the web UI may read datasets from\n"
        "  --no-open          do not launch a browser\n"
        "  --no-kill          do not let the web UI stop the server\n"
        "  --workers N        worker threads (default: one per hardware thread)\n"
        "  --partitions N     byte range partitions (default: one per worker)\n"
        "  --chunk-rows N     rows per columnar chunk (default: 32768)\n"
        "  --sample-rows N    rows to sample when profiling (default: 10000)\n"
        "  --seed S           sampling seed, fixed so runs reproduce\n"
        "  --policy P         bad rows: skip | quarantine | fail (default: skip)\n"
        "  --baseline         also time a single threaded run, for a measured speedup\n"
        "  --no-cache         ignore any cached schema and re-infer\n"
        "  --no-ai            heuristics only, do not consult the provider\n"
        "  --provider P       mock | gemini | openai | anthropic (default: mock)\n"
        "  --model M          any model id the provider accepts\n"
        "  --api-base URL     an OpenAI compatible endpoint: ollama, vLLM, OpenRouter\n"
        "  --ai-config PATH   where the provider and key are read from (default: merope.ai.json)\n"
        "  --ai-timeout N     seconds to wait for the model (default: 60)\n"
        "  --show-prompt      print exactly what is sent to the model\n";
}

bool merope::parse_arguments(int argc, char** argv, cli_arguments_t& out) {
    if (argc < 2) return false;
    out.command = argv[1];
    if (out.command == "--help" || out.command == "-h" || out.command == "help") return false;

    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value_of = [&](std::size_t& target) {
            if (index + 1 < argc) target = static_cast<std::size_t>(std::atoll(argv[++index]));
        };

        if (argument == "--workers")           value_of(out.workers);
        else if (argument == "--partitions")   value_of(out.partitions);
        else if (argument == "--chunk-rows")   value_of(out.chunk_rows);
        else if (argument == "--sample-rows")  value_of(out.sample_rows);
        else if (argument == "--seed") {
            if (index + 1 < argc) out.seed = static_cast<std::uint64_t>(std::atoll(argv[++index]));
        } else if (argument == "--rows") {
            if (index + 1 < argc) out.rows = static_cast<std::uint64_t>(std::atoll(argv[++index]));
        } else if (argument == "--corrupt") {
            if (index + 1 < argc) out.corrupt_fraction = std::atof(argv[++index]);
        } else if (argument == "--policy") {
            if (index + 1 < argc) out.policy = bad_row_policy_from_string(argv[++index]);
        } else if (argument == "--port") {
            if (index + 1 < argc) out.port = static_cast<std::uint16_t>(std::atoi(argv[++index]));
        } else if (argument == "--data") {
            if (index + 1 < argc) out.data_dir = argv[++index];
        } else if (argument == "--out") {
            if (index + 1 < argc) out.bench_output = argv[++index];
        } else if (argument == "--suite") {
            if (index + 1 < argc) out.bench_suites = argv[++index];
        } else if (argument == "--sizes") {
            if (index + 1 < argc) out.scale_sizes = argv[++index];
        } else if (argument == "--threads") {
            if (index + 1 < argc) out.thread_list = argv[++index];
        } else if (argument == "--repeats")    value_of(out.repeats);
        else if (argument == "--keep")         out.keep_datasets = true;
        else if (argument == "--no-open")      out.open_browser = false;
        else if (argument == "--no-kill")      out.allow_kill = false;
        else if (argument == "--provider") {
            if (index + 1 < argc) out.ai_provider = argv[++index];
        } else if (argument == "--model") {
            if (index + 1 < argc) out.ai_model = argv[++index];
        } else if (argument == "--api-base") {
            if (index + 1 < argc) out.ai_base = argv[++index];
        } else if (argument == "--ai-config") {
            if (index + 1 < argc) out.ai_config = argv[++index];
        } else if (argument == "--ai-timeout") {
            if (index + 1 < argc) out.ai_timeout = std::atoi(argv[++index]);
        }
        else if (argument == "--baseline")     out.measure_baseline = true;
        else if (argument == "--no-cache")     out.use_cache = false;
        else if (argument == "--confirm")      out.confirm = true;
        else if (argument == "--no-ai")        out.no_ai = true;
        else if (argument == "--show-prompt")  out.show_prompt = true;
        else if (argument == "--no-header")    out.write_header = false;
        else if (argument == "--help" || argument == "-h") return false;
        else if (argument.rfind("--", 0) == 0) {
            std::cerr << "unknown option: " << argument << "\n";
            return false;
        } else {
            out.positional.push_back(argument);
        }
    }
    return true;
}

merope::sample_options_t merope::sample_options_from(const cli_arguments_t& arguments) {
    sample_options_t options;
    options.target_rows = arguments.sample_rows;
    options.seed        = arguments.seed;
    return options;
}

merope::execution_options_t merope::execution_options_from(const cli_arguments_t& arguments) {
    execution_options_t options;
    options.workers          = arguments.workers;
    options.partitions       = arguments.partitions;
    options.chunk_rows       = arguments.chunk_rows;
    options.policy           = arguments.policy;
    options.measure_baseline = arguments.measure_baseline;
    return options;
}

static void print_dialect(const merope::sniff_result_t& sniff) {
    std::cout << "FORMAT\n";
    for (const std::string& note : sniff.notes) {
        std::cout << "  " << note << "\n";
    }
    std::cout << "  header: " << (sniff.dialect.has_header ? "yes" : "no")
              << " (confidence " << sniff.dialect.header_confidence << ")\n";
    std::cout << "  columns: " << sniff.dialect.column_count << "\n\n";
}

merope::ai_resolution_t merope::resolve_ai_from(const cli_arguments_t& arguments) {
    ai_settings_t requested;
    requested.provider        = arguments.ai_provider;
    requested.model           = arguments.ai_model;
    requested.api_base        = arguments.ai_base;
    requested.timeout_seconds = arguments.ai_timeout;
    return resolve_ai(requested, arguments.ai_config);
}

std::string merope::describe_ai(const ai_resolution_t& resolution) {
    std::string line = "provider: " + resolution.settings.provider;
    if (resolution.settings.model != resolution.settings.provider) {
        line += ":" + resolution.settings.model;
    }
    line += " (" + resolution.source + ")";
    if (resolution.remote && !resolution.settings.api_base.empty()) {
        line += " at " + resolution.settings.api_base;
    }
    if (!resolution.note.empty()) line += " - " + resolution.note;
    return line;
}

int merope::command_models(const cli_arguments_t& arguments) {
    const ai_resolution_t resolution = resolve_ai_from(arguments);
    std::cout << describe_ai(resolution) << "\n"
              << "  key: " << redact_key(resolution.settings.api_key) << "\n";
    if (!resolution.remote) {
        std::cout << "  Nothing to ask. Configure a provider in "
                  << (arguments.ai_config.empty() ? default_config_path() : arguments.ai_config)
                  << ", or pass --provider.\n";
        return 0;
    }
    std::cout << "  asking " << resolution.settings.provider
              << " which models this key may use...\n\n";

    std::vector<std::string> models;
    std::string              error;
    if (!list_models(resolution.settings, models, error)) {
        std::cerr << "could not list models: " << error << "\n";
        return 1;
    }
    for (const std::string& model : models) {
        std::cout << (model == resolution.settings.model ? "* " : "  ") << model << "\n";
    }
    std::cout << "\n" << models.size() << " model" << (models.size() == 1 ? "" : "s")
              << "; * is the one in use. Choose another with --model.\n";
    return 0;
}

int merope::command_profile(const cli_arguments_t& arguments) {
    if (arguments.positional.empty()) {
        std::cerr << "profile needs a dataset path\n";
        return 2;
    }
    const std::string& path = arguments.positional[0];

    const ai_resolution_t ai = resolve_ai_from(arguments);
    std::unique_ptr<c_ai_provider> provider;
    if (!arguments.no_ai) {
        std::cout << describe_ai(ai) << "\n";
        provider = make_ai_provider(ai.settings);
    }

    const inspection_t inspection =
        inspect_dataset(path, sample_options_from(arguments), provider.get(), false);

    print_dialect(inspection.sniff);
    std::cout << format_profile(inspection.profile);

    std::cout << "HEURISTIC INFERENCE\n" << format_hints(inspection.heuristics) << "\n";

    if (inspection.ai_ran) {
        std::cout << "PROVIDER: " << inspection.ai.provider << "\n";
        for (const std::string& note : inspection.ai.notes) std::cout << "  note: " << note << "\n";
        std::cout << format_hints(inspection.ai.proposals) << "\n";
    }

    std::cout << "INFERRED SCHEMA\n" << format_schema_table(inspection.schema) << "\n";
    if (!inspection.schema.all_confirmed()) {
        std::cout << "  This schema is a proposal. Run `merope schema " << path
                  << " --confirm` to accept it.\n";
    }

    if (arguments.show_prompt) {
        std::cout << "\nSCHEMA PROMPT (exactly what leaves this process)\n"
                  << build_schema_prompt(inspection.profile) << "\n";
    }
    return 0;
}

int merope::command_schema(const cli_arguments_t& arguments) {
    if (arguments.positional.empty()) {
        std::cerr << "schema needs a dataset path\n";
        return 2;
    }
    const std::string& path = arguments.positional[0];

    const ai_resolution_t ai = resolve_ai_from(arguments);
    std::unique_ptr<c_ai_provider> provider;
    if (!arguments.no_ai) {
        std::cout << describe_ai(ai) << "\n";
        provider = make_ai_provider(ai.settings);
    }

    inspection_t inspection =
        inspect_dataset(path, sample_options_from(arguments), provider.get(), arguments.use_cache);

    if (inspection.from_cache) {
        std::cout << "Loaded the confirmed schema from " << inspection.cache_path << "\n\n";
    }
    std::cout << format_schema_table(inspection.schema) << "\n";

    if (arguments.confirm) {
        std::string error;
        if (!confirm_and_save(inspection.schema, error)) {
            std::cerr << "could not save the schema: " << error << "\n";
            return 1;
        }
        std::cout << "Confirmed and cached at " << schema_sidecar_path(path) << "\n";
    } else if (!inspection.from_cache) {
        std::cout << "Not saved. Add --confirm to accept this schema.\n";
    }
    return 0;
}

static int report_outcome(const merope::query_outcome_t& outcome, const merope::schema_t& schema,
                          bool show_report) {
    if (!outcome.logical.operations.empty()) {
        std::cout << "LOGICAL PLAN";
        if (!outcome.logical.provider.empty()) std::cout << " (from " << outcome.logical.provider << ")";
        std::cout << "\n" << format_plan(outcome.logical) << "\n";
        std::cout << json_serialize(plan_to_json(outcome.logical), 2) << "\n\n";
    }

    for (const std::string& warning : outcome.validation.warnings) {
        std::cout << "  note: " << warning << "\n";
    }
    if (!outcome.validation.warnings.empty()) std::cout << "\n";

    if (!outcome.validation.accepted) {
        std::cout << "PLAN REJECTED\n";
        for (const std::string& error : outcome.validation.errors) {
            std::cout << "  " << error << "\n";
        }
        std::cout << "\nNothing was executed.\n";
        return 1;
    }

    std::cout << "PHYSICAL PLAN\n" << format_physical_plan(outcome.validation.plan, schema) << "\n";
    std::cout << "RESULT\n" << format_result_table(outcome.result) << "\n";
    if (show_report) std::cout << format_report(outcome.report);
    return 0;
}

int merope::command_query(const cli_arguments_t& arguments) {
    if (arguments.positional.size() < 2) {
        std::cerr << "query needs a dataset path and a question\n";
        return 2;
    }
    const std::string& path     = arguments.positional[0];
    const std::string& question = arguments.positional[1];

    const ai_resolution_t ai = resolve_ai_from(arguments);
    std::cout << describe_ai(ai) << "\n\n";
    std::unique_ptr<c_ai_provider> provider = make_ai_provider(ai.settings);
    inspection_t inspection =
        inspect_dataset(path, sample_options_from(arguments), provider.get(), arguments.use_cache);

    if (!inspection.from_cache) {
        std::cout << "Working from a proposed schema (not yet confirmed):\n"
                  << format_schema_table(inspection.schema) << "\n";
    }

    if (arguments.show_prompt) {
        std::cout << "PLAN PROMPT (exactly what leaves this process)\n"
                  << build_plan_prompt(inspection.schema, question) << "\n\n";
    }

    const query_outcome_t outcome =
        run_query(inspection.schema, *provider, question, execution_options_from(arguments));
    return report_outcome(outcome, inspection.schema, true);
}

int merope::command_plan(const cli_arguments_t& arguments) {
    if (arguments.positional.size() < 2) {
        std::cerr << "plan needs a dataset path and a plan file\n";
        return 2;
    }
    const std::string& path      = arguments.positional[0];
    const std::string& plan_file = arguments.positional[1];

    std::ifstream stream(plan_file, std::ios::binary);
    if (!stream) {
        std::cerr << "cannot open plan file: " << plan_file << "\n";
        return 2;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();

    std::unique_ptr<c_ai_provider> provider;
    if (!arguments.no_ai) provider = make_ai_provider(resolve_ai_from(arguments).settings);
    const inspection_t inspection =
        inspect_dataset(path, sample_options_from(arguments), provider.get(), arguments.use_cache);

    std::string     parse_error;
    query_outcome_t outcome =
        run_plan_json(inspection.schema, buffer.str(), execution_options_from(arguments), parse_error);
    if (!parse_error.empty()) {
        std::cerr << "the plan is not valid JSON: " << parse_error << "\n";
        return 1;
    }
    return report_outcome(outcome, inspection.schema, true);
}

int merope::command_generate(const cli_arguments_t& arguments) {
    if (arguments.positional.empty()) {
        std::cerr << "gen needs an output path\n";
        return 2;
    }
    generator_options_t options;
    options.rows             = arguments.rows;
    options.seed             = arguments.seed;
    options.write_header     = arguments.write_header;
    options.corrupt_fraction = arguments.corrupt_fraction;

    const generator_stats_t stats = generate_dataset(arguments.positional[0], options);
    std::cout << "Wrote " << format_count(stats.rows_written) << " rows, "
              << format_bytes(stats.bytes_written) << " to " << arguments.positional[0] << "\n";
    if (stats.corrupted_rows > 0) {
        std::cout << "  including " << format_count(stats.corrupted_rows) << " deliberately broken rows\n";
    }
    std::cout << "  exact sum(amount) = "
              << cell_to_display(cell_value_t{stats.total_amount_scaled}, data_type_t::decimal) << "\n";
    std::cout << "  rows in 2025      = " << format_count(stats.rows_in_2025) << "\n";
    return 0;
}

int merope::command_serve(const cli_arguments_t& arguments) {
    web_options_t web;
    web.data_root   = arguments.data_dir.empty()
                          ? std::filesystem::current_path().string()
                          : arguments.data_dir;
    web.allow_shutdown = arguments.allow_kill;
    web.ai             = resolve_ai_from(arguments);
    web.sample_rows    = arguments.sample_rows;
    web.seed        = arguments.seed;

    std::error_code ec;
    std::filesystem::create_directories(web.data_root, ec);
    if (!std::filesystem::exists(web.data_root, ec)) {
        std::cerr << "no such directory: " << web.data_root << "\n";
        return 2;
    }
    web.data_root = std::filesystem::weakly_canonical(web.data_root, ec).string();

    c_http_server server(arguments.port, arguments.workers);
    register_api(server, web);

    std::string error;
    if (!server.start(error)) {
        std::cerr << "could not start the web UI: " << error << "\n";
        return 1;
    }

    const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) + "/";
    // Flushed, because the next thing this process does is block in accept()
    // and the address is the one thing the user needs right now.
    std::cout << "merope is serving the UI at " << url << "\n"
              << "  datasets:  " << web.data_root << "\n"
              << "  " << describe_ai(web.ai) << "\n"
              << "  interface: loopback only, this machine cannot be reached from the network\n"
              << (web.allow_shutdown
                      ? "  stop with Ctrl+C, or with the kill button in the top right of the page\n"
                      : "  stop with Ctrl+C; the kill button is off (--no-kill)\n")
              << std::flush;

    if (arguments.open_browser) {
        // Quoted empty title first, so a path with spaces is not read as one.
        const std::string command = "start \"\" \"" + url + "\"";
        if (std::system(command.c_str()) != 0) {
            std::cout << "  (could not open a browser, visit the address above)\n";
        }
    }

    // Returns when the listener closes: Ctrl+C, or the kill button.
    server.run();
    std::cout << "the web UI has stopped; port " << server.port() << " is free again\n";
    return 0;
}

// Splits "1,5,10" into numbers, ignoring anything that is not one.
static std::vector<double> split_numbers(const std::string& text) {
    std::vector<double> values;
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const std::size_t end = std::min(text.find(',', cursor), text.size());
        const std::string piece = text.substr(cursor, end - cursor);
        if (!piece.empty()) {
            const double value = std::atof(piece.c_str());
            if (value > 0.0) values.push_back(value);
        }
        if (end == text.size()) break;
        cursor = end + 1;
    }
    return values;
}

static std::vector<std::string> split_words(const std::string& text) {
    std::vector<std::string> words;
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const std::size_t end = std::min(text.find(',', cursor), text.size());
        const std::string piece = text.substr(cursor, end - cursor);
        if (!piece.empty()) words.push_back(piece);
        if (end == text.size()) break;
        cursor = end + 1;
    }
    return words;
}

int merope::command_bench(const cli_arguments_t& arguments) {
    bench_options_t options;
    options.output_dir    = arguments.bench_output.empty() ? "bench" : arguments.bench_output;
    options.work_dir      = (std::filesystem::path(options.output_dir) / "data").string();
    options.seed          = arguments.seed;
    options.repeats       = arguments.repeats == 0 ? 1 : arguments.repeats;
    options.keep_datasets = arguments.keep_datasets;
    if (!arguments.positional.empty()) options.dataset = arguments.positional[0];

    options.thread_counts = {1, 2, 4, 8, 16, 32};
    if (!arguments.thread_list.empty()) {
        options.thread_counts.clear();
        for (const double value : split_numbers(arguments.thread_list)) {
            options.thread_counts.push_back(static_cast<std::size_t>(value));
        }
    }

    options.sample_sizes = {100, 1000, 10000, 100000};
    options.scale_gigabytes = {1.0, 5.0, 10.0};
    if (!arguments.scale_sizes.empty()) options.scale_gigabytes = split_numbers(arguments.scale_sizes);

    std::vector<std::string> suites = {"inference", "sampling", "threads", "scaling", "baseline"};
    if (!arguments.bench_suites.empty()) suites = split_words(arguments.bench_suites);

    return run_benchmarks(options, suites, std::cout);
}

int main(int argc, char** argv) {
    merope::cli_arguments_t arguments;
    if (!merope::parse_arguments(argc, argv, arguments)) {
        merope::print_usage();
        return argc < 2 ? 2 : 0;
    }

    try {
        if (arguments.command == "profile")  return merope::command_profile(arguments);
        if (arguments.command == "schema")   return merope::command_schema(arguments);
        if (arguments.command == "query")    return merope::command_query(arguments);
        if (arguments.command == "plan")     return merope::command_plan(arguments);
        if (arguments.command == "gen")      return merope::command_generate(arguments);
        if (arguments.command == "serve")    return merope::command_serve(arguments);
        if (arguments.command == "models")   return merope::command_models(arguments);
        if (arguments.command == "bench")    return merope::command_bench(arguments);
        if (arguments.command == "selftest") return merope::run_self_tests(std::cout);

        std::cerr << "unknown command: " << arguments.command << "\n\n";
        merope::print_usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
