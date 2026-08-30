// tests/self_test.cpp - the built in checks behind `merope selftest`.
//
// The point of most of these is that the expected answer is known independently
// of the engine: the generator records the exact totals it wrote, so a passing
// aggregation test means the engine agrees with the data, not with itself.

#include "self_test.h"

#include "../src/app/cli.h"
#include "../src/app/pipeline.h"
#include "../src/core/json.h"
#include "../src/core/parse.h"
#include "../src/dataset/generator.h"
#include "../src/engine/processing_engine.h"
#include "../src/parallel/partitioner.h"
#include "../src/plan/expression.h"
#include "../src/plan/plan_validator.h"
#include "../src/ai/http_client.h"
#include "../src/ai/remote_ai_provider.h"
#include "../src/web/api.h"
#include "../src/web/http_server.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int merope::g_checks = 0;
int merope::g_failures = 0;
std::ostream* merope::g_out = nullptr;

void merope::check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    *g_out << "  FAIL  " << what << "\n";
}

void merope::check_equal(std::int64_t actual, std::int64_t expected, const std::string& what) {
    check(actual == expected,
          what + " (expected " + std::to_string(expected) + ", got " + std::to_string(actual) + ")");
}

void merope::section(const std::string& title) {
    // Flushed so that a crash still shows how far the suite got.
    *g_out << title << std::endl;
}

std::filesystem::path merope::scratch_dir() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "merope_selftest";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    return directory;
}

void merope::write_text_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << content;
}

// ---------------------------------------------------------------------------

void merope::test_value_parsing() {
    section("value parsing");
    parse_options_t options;

    std::int64_t integral = 0;
    check(parse_int64("42", options, integral) && integral == 42, "plain integer");
    check(parse_int64("  -7 ", options, integral) && integral == -7, "signed integer with spaces");
    check(!parse_int64("19.99", options, integral), "a decimal is not an integer");
    check(!parse_int64("12abc", options, integral), "trailing junk is rejected");

    double real = 0.0;
    check(parse_float64("19.99", options, real) && std::fabs(real - 19.99) < 1e-9, "float with a dot");

    parse_options_t comma;
    comma.decimal_separator   = ',';
    comma.thousands_separator = ' ';
    check(parse_float64("1 234,50", comma, real) && std::fabs(real - 1234.50) < 1e-9,
          "float with a comma decimal and a space grouping");

    std::int64_t scaled = 0;
    check(parse_decimal("19.99", options, scaled) && scaled == 199900, "money keeps its scale");
    check(parse_decimal("-0.01", options, scaled) && scaled == -100, "negative money");
    check(parse_decimal("15.50 EUR", options, scaled) && scaled == 155000, "money with a currency code");
    check(!parse_decimal("1.234567", options, scaled),
          "money with more fraction digits than the scale is refused, not rounded");

    bool flag = false;
    check(parse_bool("TRUE", flag) && flag, "boolean true");
    check(parse_bool("0", flag) && !flag, "boolean zero");

    check(looks_like_null(""), "empty is null");
    check(looks_like_null("N/A"), "N/A is null");
    check(!looks_like_null("0"), "zero is not null");

    std::int64_t   days    = 0;
    date_pattern_t pattern = date_pattern_t::none;
    check(parse_date("2026-01-31", days, pattern) && pattern == date_pattern_t::iso, "ISO date");
    check(days == days_from_civil(2026, 1, 31), "ISO date converts to the right day number");
    check(parse_date("31.01.2026", days, pattern) && pattern == date_pattern_t::dmy_dot,
          "Central European date");
    check(days == days_from_civil(2026, 1, 31), "dotted date converts to the same day");
    check(!parse_date("2026-02-30", days, pattern), "an impossible day is rejected");

    std::int64_t seconds = 0;
    check(parse_datetime("2026-01-31T12:30:00Z", seconds, pattern), "ISO timestamp with Z");
    check(seconds == days_from_civil(2026, 1, 31) * 86400 + 12 * 3600 + 30 * 60, "timestamp seconds");
    check(parse_datetime("2026-01-31 12:30:00+01:00", seconds, pattern), "timestamp with an offset");
    check(seconds == days_from_civil(2026, 1, 31) * 86400 + 11 * 3600 + 30 * 60,
          "the offset is applied, not ignored");

    // Round trip through the whole calendar helper.
    bool calendar_ok = true;
    for (std::int64_t day = -30000; day < 30000; day += 97) {
        int year = 0; unsigned month = 0, day_of_month = 0;
        civil_from_days(day, year, month, day_of_month);
        if (days_from_civil(year, month, day_of_month) != day) calendar_ok = false;
    }
    check(calendar_ok, "civil date conversion round trips");
}

void merope::test_json() {
    section("json");
    const std::string text =
        R"({"a":1,"b":[true,null,"x\ny"],"c":{"d":-2.5},"e":"áč"})";
    json_value_t root;
    std::string  error;
    check(json_parse(text, root, error), "parses a nested document: " + error);
    check(root.int_or("a", 0) == 1, "integer member");
    check(root.find("b") != nullptr && root.find("b")->array_value.size() == 3, "array member");
    check(root.find("b")->array_value[2].string_value == "x\ny", "escaped newline in a string");
    check(root.find("c")->number_or("d", 0.0) == -2.5, "nested negative number");
    check(root.find("e")->string_value == "\xC3\xA1\xC4\x8D", "unicode escapes become UTF-8");

    json_value_t reparsed;
    check(json_parse(json_serialize(root, 2), reparsed, error), "serialize then parse: " + error);
    check(reparsed.int_or("a", 0) == 1, "round trip preserves values");

    check(!json_parse("{\"a\":}", root, error), "malformed input is rejected");
    check(!json_parse("[1,2", root, error), "unterminated array is rejected");
}

void merope::test_csv_records() {
    section("csv record splitting");
    csv_dialect_t dialect;
    dialect.delimiter = ',';

    auto fields = split_line(R"(a,"b,c",d)", dialect);
    check(fields.size() == 3 && fields[1] == "b,c", "quoted delimiter stays inside the field");

    fields = split_line(R"(a,"say ""hi""",c)", dialect);
    check(fields.size() == 3 && fields[1] == "say \"hi\"", "doubled quotes unescape");

    fields = split_line("a,,c", dialect);
    check(fields.size() == 3 && fields[1].empty(), "empty middle field is preserved");

    std::vector<std::string> record;
    std::string              buffer = "x,y\r\nz,w\r\n";
    std::size_t              cursor = 0;
    check(parse_record(buffer, cursor, dialect, record) == record_status_t::ok && record.size() == 2,
          "CRLF record");
    check(parse_record(buffer, cursor, dialect, record) == record_status_t::ok && record[0] == "z",
          "second CRLF record");
    check(parse_record(buffer, cursor, dialect, record) == record_status_t::end, "end of buffer");
}

void merope::test_sniffer(const std::filesystem::path& directory) {
    section("format sniffing");

    const std::filesystem::path semi = directory / "semicolon.csv";
    write_text_file(semi,
                    "id;amount;country\n"
                    "1;19,99;SK\n2;29,99;CZ\n3;15,50;SK\n4;101,00;DE\n5;7,25;AT\n");
    const sniff_result_t semi_result = sniff_csv(semi.string());
    check(semi_result.dialect.delimiter == ';', "semicolon delimiter detected");
    check(semi_result.dialect.decimal_separator == ',', "comma decimal separator detected");
    check(semi_result.dialect.has_header, "header detected from type contrast");
    check(semi_result.dialect.column_count == 3, "column count");

    const std::filesystem::path headerless = directory / "headerless.csv";
    write_text_file(headerless,
                    "123,42,19.99,SK,2026-01-01\n"
                    "124,17,29.99,CZ,2026-01-02\n"
                    "125,42,15.50,SK,2026-01-03\n"
                    "126,91,64.00,DE,2026-01-04\n"
                    "127,17,12.10,AT,2026-01-05\n");
    const sniff_result_t plain = sniff_csv(headerless.string());
    check(plain.dialect.delimiter == ',', "comma delimiter detected");
    check(!plain.dialect.has_header, "absent header detected");
    check(plain.dialect.column_names.size() == 5 && plain.dialect.column_names[0] == "column_0",
          "generated physical names");

    const std::filesystem::path quoted = directory / "quoted.csv";
    write_text_file(quoted, "a,b\n\"line\none\",2\n\"x\",3\n\"y\",4\n\"z\",5\n");
    const sniff_result_t quoted_result = sniff_csv(quoted.string());
    check(quoted_result.dialect.quoted_newlines, "newline inside a quoted field is noticed");

    const partition_plan_t partitions =
        plan_partitions(1000000, quoted_result.dialect, 8, 1000);
    check(partitions.partitions.size() == 1, "quoted newlines disable partitioning");
}

void merope::test_expressions() {
    section("expressions");
    std::string error;

    expr_ptr expression = parse_expression("year(timestamp) == 2025", error);
    check(expression != nullptr, "parses a function comparison: " + error);

    expression = parse_expression("country IN ('SK', 'CZ') AND amount BETWEEN 10 AND 20", error);
    check(expression != nullptr, "parses IN and BETWEEN with AND: " + error);
    std::vector<std::string> columns;
    if (expression != nullptr) collect_columns(*expression, columns);
    check(columns.size() == 2, "collects both referenced columns");

    expression = parse_expression("a IS NOT NULL OR NOT (b < 3)", error);
    check(expression != nullptr, "parses IS NOT NULL and NOT: " + error);

    check(parse_expression("year(", error) == nullptr, "unbalanced parentheses rejected");
    check(parse_expression("a ==", error) == nullptr, "dangling operator rejected");
    check(parse_expression("nosuchfn(a)", error) == nullptr, "unknown function rejected");
    check(parse_expression("", error) == nullptr, "empty expression rejected");
}

static merope::schema_t schema_for(const std::string& path) {
    const merope::inspection_t inspection =
        merope::inspect_dataset(path, merope::sample_options_t{}, nullptr, false);
    return inspection.schema;
}

void merope::test_validation(const std::filesystem::path& directory) {
    section("plan validation");

    const std::filesystem::path path = directory / "validation.csv";
    generator_options_t options;
    options.rows = 5000;
    generate_dataset(path.string(), options);

    const schema_t schema = schema_for(path.string());
    check(schema.columns.size() == 5, "five columns inferred");
    check(schema.find("amount") != nullptr &&
              schema.find("amount")->physical_type == data_type_t::decimal,
          "amount is DECIMAL, not FLOAT64");
    check(schema.find("country") != nullptr &&
              schema.find("country")->semantic_type == semantic_type_t::country,
          "country recognised");
    check(schema.find("timestamp") != nullptr &&
              schema.find("timestamp")->physical_type == data_type_t::datetime,
          "timestamp is DATETIME");

    auto validate = [&](const std::string& json) {
        query_plan_t logical;
        std::string  error;
        if (!parse_query_plan(json, logical, error)) {
            validation_result_t rejected;
            rejected.errors.push_back(error);
            return rejected;
        }
        return validate_plan(logical, schema);
    };

    // The check the spec calls out by name.
    validation_result_t outcome = validate(R"JSON({"operations":[
        {"type":"aggregate","function":"sum","column":"country","as":"x"}]})JSON");
    check(!outcome.accepted, "SUM over a COUNTRY column is rejected");

    outcome = validate(R"JSON({"operations":[
        {"type":"aggregate","function":"sum","column":"nope","as":"x"}]})JSON");
    check(!outcome.accepted, "an unknown column is rejected");

    outcome = validate(R"JSON({"operations":[
        {"type":"filter","predicate":"country == 5"}]})JSON");
    check(!outcome.accepted, "comparing a text column with a number is rejected");

    // A bare number compared against money is scaled to the column, so that
    // "amount > 10" means ten units and not a ten-thousandth of one.
    outcome = validate(R"JSON({"operations":[
        {"type":"filter","predicate":"amount > 10"},
        {"type":"aggregate","function":"count","as":"n"}]})JSON");
    check(outcome.accepted, "a number literal is scaled to a DECIMAL column");

    outcome = validate(R"JSON({"operations":[
        {"type":"filter","predicate":"amount > user_id"},
        {"type":"aggregate","function":"count","as":"n"}]})JSON");
    check(!outcome.accepted, "comparing a DECIMAL column with a plain integer column is rejected");

    outcome = validate(R"JSON({"operations":[
        {"type":"project","columns":["country"]}]})JSON");
    check(outcome.accepted, "a row listing is accepted");
    check(outcome.plan.limit == k_default_result_limit, "an unbounded row listing gets a limit");
    check(!outcome.warnings.empty(), "and the caller is told about it");

    outcome = validate(R"JSON({"operations":[
        {"type":"aggregate","function":"count","as":"n"}]})JSON");
    check(outcome.accepted && outcome.plan.limit == 0,
          "a global aggregate is provably small and needs no limit");

    outcome = validate(R"JSON({"operations":[
        {"type":"group_by","columns":["country"]},
        {"type":"aggregate","function":"avg","column":"amount","as":"mean"}]})JSON");
    check(outcome.accepted, "group by with avg is accepted");
    check(!outcome.plan.aggregates.empty() &&
              outcome.plan.aggregates[0].needs_partial_sum_and_count(),
          "AVG is marked as reducing through sum and count");

    outcome = validate(R"JSON({"operations":[
        {"type":"aggregate","function":"count","as":"n"},
        {"type":"sort","column":"nothing","order":"desc"}]})JSON");
    check(!outcome.accepted, "sorting on a non result column is rejected");

    outcome = validate(R"JSON({"operations":[{"type":"join","table":"other"}]})JSON");
    check(!outcome.accepted, "an operation outside the MVP set is rejected");

    // Only the columns actually referenced are scanned.
    outcome = validate(R"JSON({"operations":[
        {"type":"group_by","columns":["country"]},
        {"type":"aggregate","function":"sum","column":"amount","as":"revenue"}]})JSON");
    check(outcome.accepted && outcome.plan.projection.size() == 2,
          "projection pushes down to the two referenced columns");
}

void merope::test_engine(const std::filesystem::path& directory) {
    section("engine");

    const std::filesystem::path path = directory / "engine.csv";
    generator_options_t options;
    options.rows = 60000;
    options.seed = 7;
    const generator_stats_t expected = generate_dataset(path.string(), options);

    const schema_t schema = schema_for(path.string());

    auto run = [&](const std::string& json, execution_options_t execution) {
        query_plan_t logical;
        std::string  error;
        check(parse_query_plan(json, logical, error), "plan parses: " + error);
        validation_result_t validation = validate_plan(logical, schema);
        check(validation.accepted, "plan validates");
        c_processing_engine engine(schema, validation.plan, execution);
        query_result_t      result = engine.run();
        return std::make_pair(std::move(result), engine.report());
    };

    // 1. A global SUM must equal what the generator wrote, exactly.
    execution_options_t single;
    single.workers    = 1;
    single.partitions = 1;
    auto [total, total_report] = run(
        R"JSON({"operations":[{"type":"aggregate","function":"sum","column":"amount","as":"total"}]})JSON",
        single);
    check(total.rows.size() == 1, "global aggregate returns one row");
    check(total.rows[0].size() == 1 && std::holds_alternative<std::int64_t>(total.rows[0][0]),
          "sum of DECIMAL stays an exact scaled integer");
    if (!total.rows.empty() && std::holds_alternative<std::int64_t>(total.rows[0][0])) {
        check_equal(std::get<std::int64_t>(total.rows[0][0]), expected.total_amount_scaled,
                    "sum(amount) matches the generated total exactly");
    }
    check_equal(static_cast<std::int64_t>(total_report.records_processed),
                static_cast<std::int64_t>(expected.rows_written), "every row was read");

    // 2. The same query in parallel must give the same number.
    execution_options_t parallel;
    parallel.workers    = 8;
    parallel.partitions = 16;
    // The test file is small, so the minimum split has to come down with it.
    parallel.min_partition_bytes = 1;
    auto [total_parallel, parallel_report] = run(
        R"JSON({"operations":[{"type":"aggregate","function":"sum","column":"amount","as":"total"}]})JSON",
        parallel);
    check(!total_parallel.rows.empty() &&
              total_parallel.rows[0][0] == total.rows[0][0],
          "parallel sum equals the single threaded sum");
    check_equal(static_cast<std::int64_t>(parallel_report.records_processed),
                static_cast<std::int64_t>(expected.rows_written),
                "partitioned reads see every row exactly once");
    check(parallel_report.partitions > 1, "the file really was partitioned");

    // 3. Group counts must add up to the row total.
    auto [grouped, grouped_report] = run(
        R"JSON({"operations":[
            {"type":"group_by","columns":["country"]},
            {"type":"aggregate","function":"count","as":"n"},
            {"type":"aggregate","function":"sum","column":"amount","as":"revenue"},
            {"type":"sort","column":"revenue","order":"desc"},
            {"type":"limit","n":100}]})JSON",
        parallel);
    std::int64_t group_rows = 0;
    std::int64_t group_sum  = 0;
    for (const std::vector<cell_value_t>& row : grouped.rows) {
        group_rows += std::get<std::int64_t>(row[1]);
        group_sum  += std::get<std::int64_t>(row[2]);
    }
    check_equal(group_rows, static_cast<std::int64_t>(expected.rows_written),
                "group counts add up to the row total");
    check_equal(group_sum, expected.total_amount_scaled, "group sums add up to the exact total");
    check(grouped.rows.size() >= 2 && grouped.rows.size() <= 7,
          "one group per country present in the data");
    (void)grouped_report;

    bool descending = true;
    for (std::size_t index = 1; index < grouped.rows.size(); ++index) {
        if (std::get<std::int64_t>(grouped.rows[index - 1][2]) <
            std::get<std::int64_t>(grouped.rows[index][2])) {
            descending = false;
        }
    }
    check(descending, "results are sorted by revenue descending");

    // 4. A computed column, a filter over it, and AVG.
    auto [filtered, filtered_report] = run(
        R"JSON({"operations":[
            {"type":"project","expr":"year(timestamp)","as":"year"},
            {"type":"filter","predicate":"year == 2025"},
            {"type":"aggregate","function":"count","as":"n"},
            {"type":"aggregate","function":"avg","column":"amount","as":"mean"}]})JSON",
        parallel);
    check(!filtered.rows.empty(), "filtered aggregate returns a row");
    if (!filtered.rows.empty()) {
        check_equal(std::get<std::int64_t>(filtered.rows[0][0]),
                    static_cast<std::int64_t>(expected.rows_in_2025),
                    "year(timestamp) == 2025 selects exactly the 2025 rows");
    }
    check_equal(static_cast<std::int64_t>(filtered_report.rows_after_filter),
                static_cast<std::int64_t>(expected.rows_in_2025),
                "the report agrees with the filter");

    // AVG must be the mean of all values, not a mean of partition means.
    execution_options_t lopsided;
    lopsided.workers    = 4;
    lopsided.partitions = 13;  // deliberately uneven
    lopsided.min_partition_bytes = 1;
    auto [mean_many, mean_many_report] = run(
        R"JSON({"operations":[{"type":"aggregate","function":"avg","column":"amount","as":"mean"}]})JSON",
        lopsided);
    auto [mean_one, mean_one_report] = run(
        R"JSON({"operations":[{"type":"aggregate","function":"avg","column":"amount","as":"mean"}]})JSON",
        single);
    const double a = std::get<double>(mean_many.rows[0][0]);
    const double b = std::get<double>(mean_one.rows[0][0]);
    check(std::fabs(a - b) < 1e-9, "AVG is identical across uneven partition counts");
    const double expected_mean = static_cast<double>(expected.total_amount_scaled) /
                                 static_cast<double>(expected.rows_written) /
                                 static_cast<double>(k_money_factor);
    check(std::fabs(a - expected_mean) < 1e-6, "AVG matches the generated mean");
    (void)mean_many_report;
    (void)mean_one_report;

    // 5. NULL handling: country has generated nulls, count(country) < count(*).
    auto [nulls, nulls_report] = run(
        R"JSON({"operations":[
            {"type":"aggregate","function":"count","as":"rows"},
            {"type":"aggregate","function":"count","column":"country","as":"with_country"}]})JSON",
        single);
    check(std::get<std::int64_t>(nulls.rows[0][1]) < std::get<std::int64_t>(nulls.rows[0][0]),
          "count(column) skips nulls while count(*) does not");
    (void)nulls_report;
}

void merope::test_bad_rows(const std::filesystem::path& directory) {
    section("bad row policy");

    const std::filesystem::path path = directory / "corrupt.csv";
    generator_options_t options;
    options.rows             = 20000;
    options.seed             = 11;
    options.corrupt_fraction = 0.02;
    const generator_stats_t expected = generate_dataset(path.string(), options);
    check(expected.corrupted_rows > 0, "the generator produced broken rows to test with");

    const schema_t schema = schema_for(path.string());
    query_plan_t   logical;
    std::string    error;
    parse_query_plan(
        R"JSON({"operations":[{"type":"aggregate","function":"sum","column":"amount","as":"total"}]})JSON",
        logical, error);
    const validation_result_t validation = validate_plan(logical, schema);
    check(validation.accepted, "plan validates against the corrupted dataset");
    // Corrupt rows break arity, not the amount column, so the type must survive.
    check(schema.find("amount") != nullptr &&
              schema.find("amount")->physical_type == data_type_t::decimal,
          "broken rows do not change the inferred type of a clean column");
    if (!validation.accepted) return;

    execution_options_t skip;
    skip.policy              = bad_row_policy_t::skip;
    skip.workers             = 4;
    skip.partitions          = 4;
    skip.min_partition_bytes = 1;
    c_processing_engine skipping(schema, validation.plan, skip);
    const query_result_t skipped = skipping.run();
    check_equal(static_cast<std::int64_t>(skipping.report().bad_rows),
                static_cast<std::int64_t>(expected.corrupted_rows),
                "every broken row is counted");
    check_equal(static_cast<std::int64_t>(skipping.report().records_processed),
                static_cast<std::int64_t>(expected.rows_written - expected.corrupted_rows),
                "broken rows do not reach the engine");
    check(!skipped.rows.empty() &&
              std::get<std::int64_t>(skipped.rows[0][0]) == expected.total_amount_scaled,
          "the total still matches, because broken rows carried no amount");

    execution_options_t fail;
    fail.policy     = bad_row_policy_t::fail;
    fail.workers    = 1;
    fail.partitions = 1;
    bool threw = false;
    try {
        c_processing_engine failing(schema, validation.plan, fail);
        failing.run();
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "the fail policy stops the query instead of returning a partial answer");

    execution_options_t quarantine;
    quarantine.policy     = bad_row_policy_t::quarantine;
    quarantine.workers    = 2;
    quarantine.partitions = 2;
    c_processing_engine quarantining(schema, validation.plan, quarantine);
    quarantining.run();
    check(quarantining.report().quarantined_rows > 0, "quarantined rows are kept for inspection");
}

void merope::test_partition_coverage(const std::filesystem::path& directory) {
    section("partition coverage");

    const std::filesystem::path path = directory / "coverage.csv";
    generator_options_t options;
    options.rows = 25000;
    options.seed = 3;
    const generator_stats_t expected = generate_dataset(path.string(), options);

    const sniff_result_t sniff = sniff_csv(path.string());
    std::error_code      ec;
    const std::uint64_t  size = std::filesystem::file_size(path, ec);

    for (const std::size_t count : {1u, 2u, 3u, 7u, 16u, 64u}) {
        const partition_plan_t partitions = plan_partitions(size, sniff.dialect, count, 1);

        // The ranges must tile the file with no gap and no overlap.
        std::uint64_t previous = 0;
        bool          contiguous = true;
        for (const partition_t& partition : partitions.partitions) {
            if (partition.begin != previous) contiguous = false;
            previous = partition.end;
        }
        check(contiguous && previous == size,
              "partitions tile the file for " + std::to_string(count) + " splits");

        std::uint64_t rows = 0;
        std::vector<std::string> fields;
        for (const partition_t& partition : partitions.partitions) {
            c_record_reader reader(path.string(), sniff.dialect, partition.begin, partition.end,
                                   partition.begin == 0);
            while (reader.next(fields)) ++rows;
        }
        check_equal(static_cast<std::int64_t>(rows), static_cast<std::int64_t>(expected.rows_written),
                    "every data row is read exactly once with " + std::to_string(count) + " splits");
    }
}

void merope::test_web_guards(const std::filesystem::path& directory) {
    section("web guards");

    // url_decode feeds the request router, so a bad decode is a routing bug.
    check(url_decode("/api/query") == "/api/query", "plain path is unchanged");
    check(url_decode("a%20b") == "a b", "percent escape decodes");
    check(url_decode("a+b") == "a b", "plus decodes to a space");
    check(url_decode("100%") == "100%", "a trailing percent is left alone");
    check(url_decode("%zz") == "%zz", "an invalid escape is left alone");

    // The path guard is what keeps a browser tab from reading the whole disk.
    const std::filesystem::path root = directory / "served";
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    std::filesystem::create_directories(directory / "served_other", ec);
    write_text_file(root / "inside.csv", "a,b\n1,2\n");
    write_text_file(directory / "outside.csv", "a,b\n1,2\n");
    write_text_file(directory / "served_other" / "sneaky.csv", "a,b\n1,2\n");

    std::string resolved;
    std::string error;
    check(resolve_dataset_path(root.string(), "inside.csv", resolved, error),
          "a file inside the root resolves: " + error);
    check(!resolve_dataset_path(root.string(), "../outside.csv", resolved, error),
          "a relative path climbing out is refused");
    check(!resolve_dataset_path(root.string(), "./../../outside.csv", resolved, error),
          "a longer climb is refused too");
    check(!resolve_dataset_path(root.string(), (directory / "outside.csv").string(), resolved, error),
          "an absolute path outside the root is refused");
    // The sibling directory shares a name prefix with the root, which is what a
    // string comparison would get wrong.
    check(!resolve_dataset_path(root.string(), "../served_other/sneaky.csv", resolved, error),
          "a sibling directory sharing the root name prefix is refused");
    check(!resolve_dataset_path(root.string(), "", resolved, error), "an empty path is refused");
    check(!resolve_dataset_path(root.string(), "nothing_here.csv", resolved, error),
          "a file that does not exist is refused");
    check(!resolve_dataset_path(root.string(), ".", resolved, error), "a directory is not a dataset");

    // The kill button ends the process, and a form on another site can post to
    // this origin without being able to read anything back. The session token
    // is what separates the page this server sent from that form.
    const std::string token = make_session_token();
    check(token.size() == 32, "a session token is 128 bits of hex");
    check(token != make_session_token(), "two sessions do not share a token");
    check(shutdown_token_matches(token, token), "the right token is accepted");
    check(!shutdown_token_matches(token, ""), "a missing token is refused");
    check(!shutdown_token_matches(token, token.substr(0, 31)), "a truncated token is refused");
    check(!shutdown_token_matches(token, token.substr(0, 31) + "x"), "a wrong last byte is refused");
    check(!shutdown_token_matches("", ""), "an empty session token accepts nothing");

    // A refusal has to be an HTTP failure, not a 200 carrying an error.
    http_response_t response;
    response.status = 400;
    response.json("{\"ok\":false}");
    check_equal(response.status, 400, "writing a JSON body leaves the status alone");
}

void merope::test_ai_layer(const std::filesystem::path& directory) {
    section("ai adapter");

    // Where a request goes. Getting this wrong sends a key to a host nobody
    // asked for, so it is checked before anything else.
    std::string   scheme;
    std::string   host;
    std::string   path;
    std::string   error;
    std::uint16_t port = 0;
    check(split_url("https://api.openai.com/v1/chat/completions", scheme, host, port, path, error),
          "a plain https URL splits: " + error);
    check(scheme == "https" && host == "api.openai.com" && port == 443 &&
          path == "/v1/chat/completions", "scheme, host, default port and path");
    check(split_url("http://127.0.0.1:11434/v1/models", scheme, host, port, path, error) &&
          port == 11434 && host == "127.0.0.1",
          "an explicit port is used, which is what makes a local model reachable");
    check(split_url("https://example.com", scheme, host, port, path, error) && path == "/",
          "a URL with no path asks for the root");
    check(!split_url("api.openai.com/v1", scheme, host, port, path, error),
          "a URL with no scheme is refused rather than assumed");
    check(!split_url("https://example.com:port/v1", scheme, host, port, path, error),
          "a port that is not a number is refused rather than ignored");
    check(!split_url("ftp://example.com/v1", scheme, host, port, path, error),
          "a scheme this cannot speak is refused");

    // Which wire each name means, including the aliases a user will reach for.
    check(wire_from_string("gemini") == ai_wire_t::gemini, "gemini");
    check(wire_from_string("GOOGLE") == ai_wire_t::gemini, "google, case insensitively");
    check(wire_from_string("claude") == ai_wire_t::anthropic, "claude means anthropic");
    check(wire_from_string("ollama") == ai_wire_t::openai, "ollama speaks the openai shape");
    check(wire_from_string("") == ai_wire_t::mock, "nothing configured means the mock");
    check(wire_from_string("something-else") == ai_wire_t::mock,
          "an unknown provider falls back to the mock rather than to a guess");

    // Endpoints, including the one that is not built from the base alone.
    check(chat_endpoint(ai_wire_t::gemini, "", "gemini-3.6-flash") ==
              "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.6-flash:generateContent",
          "gemini puts the model in the path");
    check(chat_endpoint(ai_wire_t::openai, "", "gpt-4o-mini") ==
              "https://api.openai.com/v1/chat/completions", "the openai default endpoint");
    check(chat_endpoint(ai_wire_t::openai, "http://127.0.0.1:11434/v1/", "llama3") ==
              "http://127.0.0.1:11434/v1/chat/completions",
          "a local base URL is used, trailing slash and all");
    check(chat_endpoint(ai_wire_t::anthropic, "", "claude-sonnet-5") ==
              "https://api.anthropic.com/v1/messages", "the anthropic default endpoint");

    // A model told to answer with JSON often fences it anyway.
    check(strip_code_fence("```json\n{\"a\":1}\n```") == "{\"a\":1}", "a fenced answer is unwrapped");
    check(strip_code_fence("```\n{\"a\":1}\n```") == "{\"a\":1}", "a fence with no language tag");
    check(strip_code_fence("  {\"a\":1}  ") == "{\"a\":1}", "an unfenced answer is only trimmed");

    // Reading the reply. Each provider hides the text somewhere different, and
    // reaching into the wrong field yields an empty plan much later.
    std::string text;
    check(extract_reply_text(ai_wire_t::gemini,
                             "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"one\"},"
                             "{\"text\":\"two\"}]}}]}", text, error) && text == "onetwo",
          "gemini parts are joined in order: " + error);
    check(extract_reply_text(ai_wire_t::openai,
                             "{\"choices\":[{\"message\":{\"content\":\"hello\"}}]}", text, error) &&
              text == "hello", "the openai message content");
    check(extract_reply_text(ai_wire_t::anthropic,
                             "{\"content\":[{\"type\":\"text\",\"text\":\"hello\"}]}", text, error) &&
              text == "hello", "the anthropic text block");
    check(!extract_reply_text(ai_wire_t::openai, "{\"error\":{\"message\":\"bad key\"}}", text, error) &&
              error == "bad key",
          "the provider's own error is repeated rather than replaced with a guess");
    check(!extract_reply_text(ai_wire_t::gemini, "not json at all", text, error),
          "a reply that is not JSON is a failure, not an empty answer");
    check(!extract_reply_text(ai_wire_t::gemini, "{\"candidates\":[{\"finishReason\":\"MAX_TOKENS\"}]}",
                              text, error) && error.find("MAX_TOKENS") != std::string::npos,
          "a truncated answer says so");

    // The request body: every wire has to name the model somewhere, and the
    // system prompt has to survive the trip.
    ai_settings_t settings;
    settings.model             = "some-model";
    settings.max_output_tokens = 1234;
    for (const ai_wire_t wire : {ai_wire_t::gemini, ai_wire_t::openai, ai_wire_t::anthropic}) {
        const std::string body = build_chat_body(wire, settings, "SYSTEM", "USER");
        json_value_t      parsed;
        check(json_parse(body, parsed, error), std::string(to_string(wire)) + " builds valid JSON");
        check(body.find("SYSTEM") != std::string::npos && body.find("USER") != std::string::npos,
              std::string(to_string(wire)) + " carries both the instructions and the question");
    }
    check(build_chat_body(ai_wire_t::openai, settings, "s", "u").find("some-model") != std::string::npos,
          "the openai body names the model");
    check(build_chat_body(ai_wire_t::gemini, settings, "s", "u").find("systemInstruction") !=
              std::string::npos,
          "gemini takes the instructions in their own field");

    // A key must never be printable in full, anywhere.
    const std::string key = "sk-abcdefghijklmnop";
    check(redact_key(key).find(key) == std::string::npos, "a redacted key is not the key");
    check(redact_key(key).find("abcdefghij") == std::string::npos, "and not most of it either");
    check(redact_key("") == "(none)", "no key says so plainly");

    // Resolution. The config file is written into the scratch directory so the
    // outcome does not depend on what happens to sit in the working directory.
    const std::filesystem::path config = directory / "ai_config.json";
    write_text_file(config, "{\"provider\":\"anthropic\",\"model\":\"claude-sonnet-5\","
                            "\"api_key\":\"test-key\"}");

    ai_settings_t requested;
    const ai_resolution_t from_file = resolve_ai(requested, config.string());
    check(from_file.remote && from_file.settings.provider == "anthropic",
          "the config file selects the provider");
    check(from_file.settings.model == "claude-sonnet-5", "and the model");
    check(from_file.source == config.string(), "and says where the decision came from");

    requested.provider = "gemini";
    requested.model    = "gemini-3.6-flash";
    const ai_resolution_t from_flags = resolve_ai(requested, config.string());
    check(from_flags.settings.provider == "gemini" && from_flags.settings.model == "gemini-3.6-flash",
          "an explicit provider beats the config file");
    check(from_flags.settings.api_key == "test-key",
          "but the key in the file is still used, because there is nowhere else to put one");

    // A remote provider with no key would refuse every request. Better to say
    // so once and answer with the mock than to 401 on every column.
    ai_settings_t keyless;
    keyless.provider = "openai";
    const ai_resolution_t no_key = resolve_ai(keyless, (directory / "no_such_config.json").string());
    check(!no_key.remote, "a provider with no key does not become the provider");
    check(no_key.settings.provider == "mock", "the mock answers instead");
    check(!no_key.note.empty(), "and the reason is stated rather than left to be discovered");

    // The page may choose a model. It may not choose a provider, an endpoint or
    // a key: those belong to whoever started the server.
    web_options_t options;
    options.ai                  = from_file;
    options.ai.settings.api_key = "test-key";
    const ai_settings_t chosen  = settings_for_request(options, "claude-opus-5");
    check(chosen.model == "claude-opus-5", "a request may name another model");
    check(chosen.provider == "anthropic" && chosen.api_key == "test-key",
          "but not another provider, and not another key");
    check(settings_for_request(options, "").model == "claude-sonnet-5",
          "naming no model keeps the configured one");
}

void merope::test_end_to_end(const std::filesystem::path& directory) {
    section("end to end");

    // Headerless, exactly the shape of the example in the specification.
    const std::filesystem::path path = directory / "headerless_full.csv";
    generator_options_t options;
    options.rows         = 30000;
    options.seed         = 5;
    options.write_header = false;
    const generator_stats_t expected = generate_dataset(path.string(), options);

    std::unique_ptr<c_ai_provider> provider = make_mock_provider();
    inspection_t inspection = inspect_dataset(path.string(), sample_options_t{}, provider.get(), false);

    check(!inspection.schema.dialect.has_header, "headerless file recognised");
    check(inspection.schema.find("amount") != nullptr, "the provider named the monetary column");
    check(inspection.schema.find("country") != nullptr, "the provider named the country column");
    check(inspection.schema.find("timestamp") != nullptr, "the provider named the timestamp column");

    // Confirming and reloading must reproduce the same schema.
    std::string error;
    check(confirm_and_save(inspection.schema, error), "schema saves: " + error);
    schema_t reloaded;
    check(load_schema(schema_sidecar_path(path.string()), reloaded, error), "schema reloads: " + error);
    reloaded.dataset_path = path.string();
    check(reloaded.columns.size() == inspection.schema.columns.size(), "reloaded column count");
    check(reloaded.all_confirmed(), "reloaded schema is marked confirmed");
    check(reloaded.find("amount") != nullptr &&
              reloaded.find("amount")->physical_type == data_type_t::decimal,
          "reloaded types survive the round trip");

    execution_options_t execution;
    execution.workers    = 4;
    execution.partitions = 8;
    const query_outcome_t outcome =
        run_query(reloaded, *provider, "Show total amount by country in 2025", execution);
    check(outcome.accepted, "the natural language query produced an executable plan");
    if (outcome.accepted) {
        check(!outcome.result.rows.empty(), "the query returned rows");
        std::int64_t rows_seen = static_cast<std::int64_t>(outcome.report.rows_after_filter);
        check_equal(rows_seen, static_cast<std::int64_t>(expected.rows_in_2025),
                    "the 2025 filter selected the right number of rows");
        check(outcome.result.columns.size() == 2, "country plus the aggregate");
    }

    std::error_code ec;
    std::filesystem::remove(schema_sidecar_path(path.string()), ec);
}

int merope::run_self_tests(std::ostream& out) {
    g_out      = &out;
    g_checks   = 0;
    g_failures = 0;

    const std::filesystem::path directory = scratch_dir();
    out << "merope self test\n"
        << "scratch directory: " << directory.string() << "\n\n";

    try {
        test_value_parsing();
        test_json();
        test_csv_records();
        test_sniffer(directory);
        test_expressions();
        test_validation(directory);
        test_engine(directory);
        test_bad_rows(directory);
        test_partition_coverage(directory);
        test_web_guards(directory);
        test_ai_layer(directory);
        test_end_to_end(directory);
    } catch (const std::exception& error) {
        out << "  FAIL  unexpected exception: " << error.what() << "\n";
        ++g_failures;
    }

    out << "\n" << (g_checks - g_failures) << " / " << g_checks << " checks passed\n";
    if (g_failures > 0) out << g_failures << " FAILURE(S)\n";
    return g_failures == 0 ? 0 : 1;
}

