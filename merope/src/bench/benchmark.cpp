#include "benchmark.h"

#include "../app/pipeline.h"
#include "../dataset/generator.h"
#include "../engine/execution_report.h"
#include "../engine/processing_engine.h"
#include "../plan/plan_validator.h"
#include "../plan/query_plan.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#endif

static const char* k_workload_plan = R"PLAN({"operations":[
  {"type":"project","expr":"year(timestamp)","as":"year"},
  {"type":"filter","predicate":"year == 2025"},
  {"type":"group_by","columns":["country"]},
  {"type":"aggregate","function":"sum","column":"amount","as":"revenue"},
  {"type":"sort","column":"revenue","order":"desc"}]})PLAN";

static const char* k_workload_sql =
    "SELECT country, sum(amount) AS revenue\n"
    "FROM read_csv_auto('{PATH}', header=true)\n"
    "WHERE year(timestamp) = 2025\n"
    "GROUP BY country\n"
    "ORDER BY revenue DESC;\n";

static const char* k_workload_polars =
    "import sys, time\n"
    "import polars as pl\n"
    "path = sys.argv[1]\n"
    "start = time.perf_counter()\n"
    "frame = (pl.scan_csv(path, try_parse_dates=True)\n"
    "           .filter(pl.col('timestamp').dt.year() == 2025)\n"
    "           .group_by('country')\n"
    "           .agg(pl.col('amount').sum().alias('revenue'))\n"
    "           .sort('revenue', descending=True)\n"
    "           .collect())\n"
    "print(f'{time.perf_counter() - start:.4f}')\n"
    "print(frame)\n";

// ---------------------------------------------------------------- helpers ---

static double median_of(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    if (values.size() % 2 == 1) return values[middle];
    return (values[middle - 1] + values[middle]) / 2.0;
}

static std::string timestamp_tag() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#ifdef _WIN32
    localtime_s(&parts, &now);
#else
    parts = *std::localtime(&now);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &parts);
    return buffer;
}

static std::string fixed(double value, int places) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(places) << value;
    return out.str();
}

// A tiny CSV writer: the results are meant to be opened in something else.
class c_csv_writer {
public:
    c_csv_writer(const std::string& path, const std::string& header) : m_stream(path, std::ios::trunc) {
        m_stream << header << "\n";
    }
    // Flushed per row: a scaling run can take half an hour, and losing the
    // small sizes because the largest one failed would be miserable.
    void row(const std::string& line) { m_stream << line << "\n" << std::flush; }
    bool ok() const { return static_cast<bool>(m_stream); }

private:
    std::ofstream m_stream;
};

merope::machine_info_t merope::describe_machine() {
    machine_info_t info;
    info.logical_cores = default_worker_count();

#ifdef _WIN32
    // CPU brand string, straight from the processor.
    int registers[4] = {0};
    char brand[0x40] = {0};
    __cpuid(registers, 0x80000000);
    if (static_cast<unsigned>(registers[0]) >= 0x80000004u) {
        for (unsigned leaf = 0; leaf < 3; ++leaf) {
            __cpuid(registers, static_cast<int>(0x80000002u + leaf));
            std::memcpy(brand + leaf * sizeof(registers), registers, sizeof(registers));
        }
        info.cpu = brand;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) info.memory_bytes = memory.ullTotalPhys;
    info.os = "Windows";
#endif

    // Trim the padding the brand string comes with.
    while (!info.cpu.empty() && (info.cpu.back() == ' ' || info.cpu.back() == '\0')) info.cpu.pop_back();
    if (info.cpu.empty()) info.cpu = "unknown";
    if (info.os.empty()) info.os = "unknown";
    return info;
}

std::string merope::describe_build() {
    std::ostringstream out;
#if defined(_MSC_FULL_VER)
    out << "MSVC " << _MSC_FULL_VER;
#else
    out << "unknown compiler";
#endif
#if defined(_MSVC_LANG)
    out << ", _MSVC_LANG=" << _MSVC_LANG;
#endif
#if defined(NDEBUG)
    out << ", NDEBUG";
#else
    out << ", _DEBUG";
#endif
#if defined(MEROPE_BUILD_CONFIG)
    out << ", config=" << MEROPE_BUILD_CONFIG;
#endif
    out << " (flags are in merope.vcxproj)";
    return out.str();
}

// --------------------------------------------------------------- plumbing ---


struct workload_result_t {
    double        seconds       = 0.0;
    std::uint64_t records       = 0;
    std::uint64_t after_filter  = 0;
    std::uint64_t bytes_scanned = 0;
    std::uint64_t peak_bytes    = 0;
    std::size_t   groups        = 0;
};


// Runs the fixed workload once, measuring this run's own memory peak rather
// than the process wide counter, which never comes back down.
static workload_result_t run_workload(const merope::schema_t& schema, std::size_t workers,
                                      std::size_t partitions) {
    merope::query_plan_t logical;
    std::string          error;
    if (!merope::parse_query_plan(std::string(k_workload_plan), logical, error)) {
        throw std::runtime_error("benchmark plan is malformed: " + error);
    }
    const merope::validation_result_t validation = merope::validate_plan(logical, schema);
    if (!validation.accepted) {
        throw std::runtime_error("benchmark plan was rejected: " +
                                 (validation.errors.empty() ? std::string("unknown reason")
                                                            : validation.errors.front()));
    }

    merope::execution_options_t execution;
    execution.workers             = workers;
    execution.partitions          = partitions;
    execution.min_partition_bytes = 1;   // honour the requested split even on small files

    merope::c_memory_sampler sampler(4);
    sampler.start();
    merope::c_processing_engine engine(schema, validation.plan, execution);
    const merope::query_result_t result = engine.run();
    sampler.stop();

    workload_result_t out;
    out.seconds       = engine.report().processing_seconds;
    out.records       = engine.report().records_processed;
    out.after_filter  = engine.report().rows_after_filter;
    out.bytes_scanned = engine.report().bytes_scanned;
    out.peak_bytes    = sampler.peak();
    out.groups        = result.rows.size();
    return out;
}


struct inference_score_t {
    double      type_accuracy     = 0.0;
    double      semantic_accuracy = 0.0;
    double      mean_confidence   = 0.0;
    double      unknown_rate      = 0.0;
    std::size_t columns           = 0;
};


// Scores an inferred schema against what the generator actually wrote.
static inference_score_t score_inference(const merope::schema_t& schema) {
    std::size_t truth_count = 0;
    const merope::generated_column_t* truth = merope::generated_schema(truth_count);

    inference_score_t score;
    score.columns = schema.columns.size();
    if (score.columns == 0) return score;

    std::size_t type_hits     = 0;
    std::size_t semantic_hits = 0;
    std::size_t unknowns      = 0;
    double      confidence    = 0.0;

    for (const merope::column_schema_t& column : schema.columns) {
        if (column.physical_index < truth_count) {
            const merope::generated_column_t& expected = truth[column.physical_index];
            if (std::string(merope::to_string(column.physical_type)) == expected.physical_type) {
                ++type_hits;
            }
            if (std::string(merope::to_string(column.semantic_type)) == expected.semantic_type) {
                ++semantic_hits;
            }
        }
        if (column.semantic_type == merope::semantic_type_t::unknown) ++unknowns;
        confidence += column.confidence;
    }

    const double total = static_cast<double>(score.columns);
    score.type_accuracy     = static_cast<double>(type_hits) / total;
    score.semantic_accuracy = static_cast<double>(semantic_hits) / total;
    score.unknown_rate      = static_cast<double>(unknowns) / total;
    score.mean_confidence   = confidence / total;
    return score;
}


struct inference_run_t {
    merope::inspection_t inspection;
    inference_score_t    score;
    double               milliseconds = 0.0;
};


static inference_run_t infer_once(const std::string& path, std::size_t sample_rows,
                                  std::uint64_t seed, bool with_ai, bool force_offsets) {
    merope::sample_options_t sample;
    sample.target_rows = sample_rows;
    sample.seed        = seed;
    // The sampler picks its method by file size; the benchmark needs to pin it.
    sample.full_scan_limit_bytes = force_offsets ? 0 : static_cast<std::uint64_t>(-1);

    std::unique_ptr<merope::c_ai_provider> provider;
    if (with_ai) provider = merope::make_mock_provider();

    const auto started = std::chrono::steady_clock::now();
    inference_run_t run;
    run.inspection = merope::inspect_dataset(path, sample, provider.get(), false);
    run.milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    run.score = score_inference(run.inspection.schema);
    return run;
}

// Generates a dataset unless one was supplied, and reports what it wrote.
static merope::generator_stats_t ensure_dataset(const std::string& path, std::uint64_t rows,
                                                std::uint64_t seed, bool with_header,
                                                double& generate_seconds) {
    merope::generator_options_t options;
    options.rows         = rows;
    options.seed         = seed;
    options.write_header = with_header;

    const auto started = std::chrono::steady_clock::now();
    const merope::generator_stats_t stats = merope::generate_dataset(path, options);
    generate_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

// ----------------------------------------------------------------- suites ---

static void suite_inference(const merope::bench_options_t& options, const std::string& run_dir,
                            std::ostream& log) {
    log << "12.1 schema inference\n";

    const std::filesystem::path with_header    = std::filesystem::path(options.work_dir) / "infer_header.csv";
    const std::filesystem::path without_header = std::filesystem::path(options.work_dir) / "infer_bare.csv";

    double generate_seconds = 0.0;
    ensure_dataset(with_header.string(), options.inference_rows, options.seed, true, generate_seconds);
    ensure_dataset(without_header.string(), options.inference_rows, options.seed, false, generate_seconds);

    c_csv_writer csv((std::filesystem::path(run_dir) / "inference.csv").string(),
                     "dataset,header,mode,sample_method,sample_rows,type_accuracy,"
                     "semantic_accuracy,mean_confidence,unknown_rate,inference_ms");

    for (const bool header : {true, false}) {
        const std::string path = header ? with_header.string() : without_header.string();
        for (const bool with_ai : {false, true}) {
            for (const bool offsets : {false, true}) {
                const inference_run_t run =
                    infer_once(path, 10000, options.seed, with_ai, offsets);

                std::ostringstream row;
                row << (header ? "generated_header" : "generated_headerless") << ","
                    << (header ? "yes" : "no") << ","
                    << (with_ai ? "heuristics+ai" : "heuristics") << ","
                    << (offsets ? "head+offsets" : "reservoir") << ","
                    << 10000 << ","
                    << fixed(run.score.type_accuracy, 4) << ","
                    << fixed(run.score.semantic_accuracy, 4) << ","
                    << fixed(run.score.mean_confidence, 4) << ","
                    << fixed(run.score.unknown_rate, 4) << ","
                    << fixed(run.milliseconds, 2);
                csv.row(row.str());

                log << "  " << (header ? "header    " : "headerless")
                    << "  " << (with_ai ? "heuristics+ai" : "heuristics   ")
                    << "  " << (offsets ? "offsets  " : "reservoir")
                    << "  type " << fixed(run.score.type_accuracy * 100, 1) << "%"
                    << "  semantic " << fixed(run.score.semantic_accuracy * 100, 1) << "%"
                    << "  unknown " << fixed(run.score.unknown_rate * 100, 1) << "%"
                    << "  " << fixed(run.milliseconds, 1) << " ms\n";
            }
        }
    }

    if (!options.keep_datasets) {
        std::error_code ec;
        std::filesystem::remove(with_header, ec);
        std::filesystem::remove(without_header, ec);
        std::filesystem::remove(merope::schema_sidecar_path(with_header.string()), ec);
        std::filesystem::remove(merope::schema_sidecar_path(without_header.string()), ec);
    }
    log << "\n";
}

static void suite_sampling(const merope::bench_options_t& options, const std::string& run_dir,
                           std::ostream& log) {
    log << "12.2 sampling\n";

    const std::filesystem::path path = std::filesystem::path(options.work_dir) / "sampling.csv";
    double generate_seconds = 0.0;
    const merope::generator_stats_t truth =
        ensure_dataset(path.string(), options.inference_rows, options.seed, true, generate_seconds);

    c_csv_writer csv((std::filesystem::path(run_dir) / "sampling.csv").string(),
                     "sample_rows,sample_method,type_accuracy,semantic_accuracy,"
                     "amount_min_observed,amount_min_true,amount_max_observed,amount_max_true,"
                     "amount_max_error_pct,country_distinct_observed,country_distinct_true,"
                     "id_unique_ratio,inference_ms");

    const double true_min = static_cast<double>(truth.min_amount_scaled) / merope::k_money_factor;
    const double true_max = static_cast<double>(truth.max_amount_scaled) / merope::k_money_factor;

    for (const std::size_t rows : options.sample_sizes) {
        for (const bool offsets : {false, true}) {
            const inference_run_t run = infer_once(path.string(), rows, options.seed, true, offsets);

            double observed_min = 0.0;
            double observed_max = 0.0;
            std::size_t country_distinct = 0;
            double id_unique_ratio = 0.0;
            for (const merope::column_profile_t& column : run.inspection.profile.columns) {
                if (column.physical_name == "amount") {
                    if (column.has_min) {
                        observed_min = static_cast<double>(std::get<std::int64_t>(column.min_value)) /
                                       merope::k_money_factor;
                    }
                    if (column.has_max) {
                        observed_max = static_cast<double>(std::get<std::int64_t>(column.max_value)) /
                                       merope::k_money_factor;
                    }
                } else if (column.physical_name == "country") {
                    country_distinct = column.unique_count;
                } else if (column.physical_name == "transaction_id") {
                    id_unique_ratio = column.unique_ratio;
                }
            }
            const double max_error =
                true_max > 0.0 ? std::fabs(observed_max - true_max) / true_max * 100.0 : 0.0;

            std::ostringstream row;
            row << rows << "," << (offsets ? "head+offsets" : "reservoir") << ","
                << fixed(run.score.type_accuracy, 4) << ","
                << fixed(run.score.semantic_accuracy, 4) << ","
                << fixed(observed_min, 4) << "," << fixed(true_min, 4) << ","
                << fixed(observed_max, 4) << "," << fixed(true_max, 4) << ","
                << fixed(max_error, 3) << ","
                << country_distinct << "," << truth.distinct_countries << ","
                << fixed(id_unique_ratio, 4) << ","
                << fixed(run.milliseconds, 2);
            csv.row(row.str());

            log << "  " << std::setw(7) << rows << " rows  "
                << (offsets ? "offsets  " : "reservoir")
                << "  type " << fixed(run.score.type_accuracy * 100, 1) << "%"
                << "  max(amount) off by " << fixed(max_error, 2) << "%"
                << "  countries " << country_distinct << "/" << truth.distinct_countries << "\n";
        }
    }

    if (!options.keep_datasets) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(merope::schema_sidecar_path(path.string()), ec);
    }
    log << "\n";
}

static void suite_threads(const merope::bench_options_t& options, const std::string& run_dir,
                          std::ostream& log) {
    log << "12.3 parallel processing\n";

    std::filesystem::path path;
    bool generated = false;
    if (!options.dataset.empty()) {
        path = options.dataset;
    } else {
        path = std::filesystem::path(options.work_dir) / "threads.csv";
        double generate_seconds = 0.0;
        ensure_dataset(path.string(), options.threads_rows, options.seed, true, generate_seconds);
        generated = true;
        log << "  wrote " << path.filename().string() << " in " << fixed(generate_seconds, 1) << " s\n";
    }

    std::unique_ptr<merope::c_ai_provider> provider = merope::make_mock_provider();
    merope::sample_options_t sample;
    sample.seed = options.seed;
    merope::inspection_t inspection = merope::inspect_dataset(path.string(), sample, provider.get(), false);
    const merope::schema_t& schema = inspection.schema;

    c_csv_writer csv((std::filesystem::path(run_dir) / "threads.csv").string(),
                     "workers,partitions,repeats,seconds_median,seconds_best,speedup,efficiency,"
                     "throughput_mb_s,peak_memory_mb,records,rows_after_filter,groups");

    double single_thread_seconds = 0.0;
    for (const std::size_t workers : options.thread_counts) {
        std::vector<double> timings;
        workload_result_t   last;
        for (std::size_t repeat = 0; repeat < options.repeats; ++repeat) {
            last = run_workload(schema, workers, workers);
            timings.push_back(last.seconds);
        }
        const double seconds = median_of(timings);
        const double best    = *std::min_element(timings.begin(), timings.end());
        if (workers == options.thread_counts.front()) single_thread_seconds = seconds;

        const double speedup    = seconds > 0.0 ? single_thread_seconds / seconds : 0.0;
        const double efficiency = workers > 0 ? speedup / static_cast<double>(workers) : 0.0;
        const double throughput = seconds > 0.0
                                      ? static_cast<double>(last.bytes_scanned) / seconds / (1024.0 * 1024.0)
                                      : 0.0;

        std::ostringstream row;
        row << workers << "," << workers << "," << options.repeats << ","
            << fixed(seconds, 4) << "," << fixed(best, 4) << ","
            << fixed(speedup, 3) << "," << fixed(efficiency, 3) << ","
            << fixed(throughput, 1) << ","
            << fixed(static_cast<double>(last.peak_bytes) / (1024.0 * 1024.0), 2) << ","
            << last.records << "," << last.after_filter << "," << last.groups;
        csv.row(row.str());

        log << "  " << std::setw(2) << workers << " workers  "
            << fixed(seconds, 3) << " s  speedup " << fixed(speedup, 2) << "x"
            << "  efficiency " << fixed(efficiency, 2)
            << "  " << fixed(throughput, 0) << " MB/s"
            << "  peak " << fixed(static_cast<double>(last.peak_bytes) / (1024.0 * 1024.0), 0) << " MB\n";
    }

    if (generated && !options.keep_datasets) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(merope::schema_sidecar_path(path.string()), ec);
    }
    log << "\n";
}

static void suite_scaling(const merope::bench_options_t& options, const std::string& run_dir,
                          std::ostream& log) {
    log << "12.4 dataset size\n";

    c_csv_writer csv((std::filesystem::path(run_dir) / "scaling.csv").string(),
                     "target_gb,bytes,rows,generate_seconds,workers,seconds,throughput_mb_s,"
                     "peak_memory_mb,records,rows_after_filter,sum_matches_truth");

    // About 43 bytes per generated row, measured; used only to hit a size target.
    constexpr double k_bytes_per_row = 43.0;
    const std::size_t workers = merope::default_worker_count();

    for (const double gigabytes : options.scale_gigabytes) {
        const std::uint64_t rows =
            static_cast<std::uint64_t>(gigabytes * 1024.0 * 1024.0 * 1024.0 / k_bytes_per_row);
        const std::filesystem::path path =
            std::filesystem::path(options.work_dir) / ("scale_" + fixed(gigabytes, 0) + "gb.csv");

        log << "  " << fixed(gigabytes, 0) << " GB target: writing " << rows << " rows...\n";
        log.flush();

        double generate_seconds = 0.0;
        const merope::generator_stats_t truth =
            ensure_dataset(path.string(), rows, options.seed, true, generate_seconds);

        std::unique_ptr<merope::c_ai_provider> provider = merope::make_mock_provider();
        merope::sample_options_t sample;
        sample.seed = options.seed;
        merope::inspection_t inspection =
            merope::inspect_dataset(path.string(), sample, provider.get(), false);

        const workload_result_t run = run_workload(inspection.schema, workers, workers);
        const double throughput = run.seconds > 0.0
                                      ? static_cast<double>(run.bytes_scanned) / run.seconds / (1024.0 * 1024.0)
                                      : 0.0;
        const bool matches = run.after_filter == truth.rows_in_2025;

        std::ostringstream row;
        row << fixed(gigabytes, 0) << "," << truth.bytes_written << "," << truth.rows_written << ","
            << fixed(generate_seconds, 2) << "," << workers << ","
            << fixed(run.seconds, 4) << "," << fixed(throughput, 1) << ","
            << fixed(static_cast<double>(run.peak_bytes) / (1024.0 * 1024.0), 2) << ","
            << run.records << "," << run.after_filter << "," << (matches ? "yes" : "NO");
        csv.row(row.str());

        log << "    " << merope::format_bytes(truth.bytes_written) << "  "
            << fixed(run.seconds, 2) << " s  " << fixed(throughput, 0) << " MB/s"
            << "  peak " << fixed(static_cast<double>(run.peak_bytes) / (1024.0 * 1024.0), 0) << " MB"
            << "  filter matches truth: " << (matches ? "yes" : "NO") << "\n";
        log.flush();

        if (!options.keep_datasets) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            std::filesystem::remove(merope::schema_sidecar_path(path.string()), ec);
        }
    }
    log << "\n";
}

// Looks for a tool on PATH by asking the shell to run it quietly.
static bool tool_available(const std::string& probe) {
    return std::system((probe + " > nul 2> nul").c_str()) == 0;
}

static void suite_baseline(const merope::bench_options_t& options, const std::string& run_dir,
                           std::ostream& log) {
    log << "12.5 baseline\n";

    std::filesystem::path path;
    bool generated = false;
    if (!options.dataset.empty()) {
        path = options.dataset;
    } else {
        path = std::filesystem::path(options.work_dir) / "baseline.csv";
        double generate_seconds = 0.0;
        ensure_dataset(path.string(), options.threads_rows, options.seed, true, generate_seconds);
        generated = true;
    }

    // Write the scripts either way, so the comparison can be run by hand later.
    std::string sql = k_workload_sql;
    const std::string needle = "{PATH}";
    std::string forward = path.string();
    std::replace(forward.begin(), forward.end(), '\\', '/');
    sql.replace(sql.find(needle), needle.size(), forward);

    const std::filesystem::path sql_path    = std::filesystem::path(run_dir) / "baseline_duckdb.sql";
    const std::filesystem::path polars_path = std::filesystem::path(run_dir) / "baseline_polars.py";
    { std::ofstream out(sql_path, std::ios::trunc);    out << sql; }
    { std::ofstream out(polars_path, std::ios::trunc); out << k_workload_polars; }

    c_csv_writer csv((std::filesystem::path(run_dir) / "baseline.csv").string(),
                     "tool,available,seconds,throughput_mb_s,note");

    std::error_code ec;
    const std::uint64_t bytes = std::filesystem::file_size(path, ec);

    // merope itself, on the same file, for the comparison row.
    {
        std::unique_ptr<merope::c_ai_provider> provider = merope::make_mock_provider();
        merope::sample_options_t sample;
        sample.seed = options.seed;
        merope::inspection_t inspection =
            merope::inspect_dataset(path.string(), sample, provider.get(), false);

        std::vector<double> timings;
        workload_result_t   last;
        for (std::size_t repeat = 0; repeat < options.repeats; ++repeat) {
            last = run_workload(inspection.schema, merope::default_worker_count(),
                                merope::default_worker_count());
            timings.push_back(last.seconds);
        }
        const double seconds = median_of(timings);
        const double throughput =
            seconds > 0.0 ? static_cast<double>(bytes) / seconds / (1024.0 * 1024.0) : 0.0;
        csv.row("merope,yes," + fixed(seconds, 4) + "," + fixed(throughput, 1) +
                ",median of " + std::to_string(options.repeats) + " runs");
        log << "  merope   " << fixed(seconds, 3) << " s  " << fixed(throughput, 0) << " MB/s\n";
    }

    const bool have_duckdb = tool_available("duckdb -c \"select 1\"");
    if (have_duckdb) {
        const auto started = std::chrono::steady_clock::now();
        const std::string command = "duckdb -c \".read " + sql_path.string() + "\" > nul";
        const int status = std::system(command.c_str());
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const double throughput =
            seconds > 0.0 ? static_cast<double>(bytes) / seconds / (1024.0 * 1024.0) : 0.0;
        csv.row(std::string("duckdb,yes,") + fixed(seconds, 4) + "," + fixed(throughput, 1) + "," +
                (status == 0 ? "one run, includes CSV sniffing" : "non zero exit"));
        log << "  duckdb   " << fixed(seconds, 3) << " s  " << fixed(throughput, 0) << " MB/s\n";
    } else {
        csv.row("duckdb,no,,,not on PATH; run baseline_duckdb.sql to fill this in");
        log << "  duckdb   not installed - wrote " << sql_path.filename().string() << " to run later\n";
    }

    const bool have_polars = tool_available("python -c \"import polars\"");
    if (have_polars) {
        const auto started = std::chrono::steady_clock::now();
        const std::string command = "python \"" + polars_path.string() + "\" \"" + path.string() + "\" > nul";
        const int status = std::system(command.c_str());
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const double throughput =
            seconds > 0.0 ? static_cast<double>(bytes) / seconds / (1024.0 * 1024.0) : 0.0;
        csv.row(std::string("polars,yes,") + fixed(seconds, 4) + "," + fixed(throughput, 1) + "," +
                (status == 0 ? "one run, includes interpreter startup" : "non zero exit"));
        log << "  polars   " << fixed(seconds, 3) << " s  " << fixed(throughput, 0) << " MB/s\n";
    } else {
        csv.row("polars,no,,,python with polars not available; run baseline_polars.py to fill this in");
        log << "  polars   not installed - wrote " << polars_path.filename().string() << " to run later\n";
    }

    if (generated && !options.keep_datasets) {
        std::filesystem::remove(path, ec);
        std::filesystem::remove(merope::schema_sidecar_path(path.string()), ec);
    }
    log << "\n";
}

// --------------------------------------------------------------- manifest ---

static void write_manifest(const merope::bench_options_t& options, const std::string& run_dir,
                           const std::vector<std::string>& suites) {
    const merope::machine_info_t machine = merope::describe_machine();
    std::ofstream out((std::filesystem::path(run_dir) / "manifest.txt").string(), std::ios::trunc);

    out << "merope benchmark run\n"
        << "====================\n\n"
        << "when            " << timestamp_tag() << "\n"
        << "suites          ";
    for (std::size_t index = 0; index < suites.size(); ++index) {
        if (index > 0) out << ", ";
        out << suites[index];
    }
    out << "\n"
        << "sample seed     " << options.seed << "\n"
        << "repeats         " << options.repeats << "\n"
        << "ai provider     mock (deterministic; no model version to record)\n"
        << "build           " << merope::describe_build() << "\n"
        << "cpu             " << machine.cpu << "\n"
        << "logical cores   " << machine.logical_cores << "\n"
        << "memory          " << merope::format_bytes(machine.memory_bytes) << "\n"
        << "os              " << machine.os << "\n\n"
        << "workload        the plan below, run directly so no model is in the timing loop\n\n"
        << k_workload_plan << "\n\n"
        << "notes\n"
        << "  - Timings are the median of the repeat count above.\n"
        << "  - Peak memory is sampled per run; the process wide counter only rises.\n"
        << "  - The generator records the true sum, row count and extremes, so accuracy\n"
        << "    columns compare against the data rather than against another engine run.\n"
        << "  - 12.1 has two arms, not three: with a mock provider the AI proposal is\n"
        << "    derived from the heuristics, so an AI-only arm needs a real adapter.\n";
}

// ------------------------------------------------------------------ entry ---

int merope::run_benchmarks(const bench_options_t& options, const std::vector<std::string>& suites,
                           std::ostream& log) {
    std::error_code ec;
    std::filesystem::create_directories(options.output_dir, ec);
    std::filesystem::create_directories(options.work_dir, ec);

    const std::string run_dir =
        (std::filesystem::path(options.output_dir) / ("run-" + timestamp_tag())).string();
    std::filesystem::create_directories(run_dir, ec);
    if (ec) {
        log << "cannot create " << run_dir << "\n";
        return 1;
    }

    const machine_info_t machine = describe_machine();
    log << "merope benchmark\n"
        << "  machine   " << machine.cpu << ", " << machine.logical_cores << " logical cores, "
        << format_bytes(machine.memory_bytes) << "\n"
        << "  build     " << describe_build() << "\n"
        << "  seed      " << options.seed << ", " << options.repeats << " repeats\n"
        << "  results   " << run_dir << "\n\n";
    log.flush();

    auto wanted = [&suites](const char* name) {
        return std::find(suites.begin(), suites.end(), std::string(name)) != suites.end();
    };

    try {
        if (wanted("inference")) suite_inference(options, run_dir, log);
        if (wanted("sampling"))  suite_sampling(options, run_dir, log);
        if (wanted("threads"))   suite_threads(options, run_dir, log);
        if (wanted("scaling"))   suite_scaling(options, run_dir, log);
        if (wanted("baseline"))  suite_baseline(options, run_dir, log);
    } catch (const std::exception& error) {
        log << "benchmark failed: " << error.what() << "\n";
        write_manifest(options, run_dir, suites);
        return 1;
    }

    write_manifest(options, run_dir, suites);
    log << "wrote results to " << run_dir << "\n";
    return 0;
}
