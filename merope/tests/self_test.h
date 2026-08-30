// tests/self_test.h - declarations for the built in check suite.
//
// The suite is one translation unit; these live in a header so that self_test.cpp
// can hold definitions only, and so the bodies keep namespace lookup.
#pragma once

#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>

namespace merope {

// Running totals for the suite.
extern int           g_checks;
extern int           g_failures;
extern std::ostream* g_out;

void check(bool condition, const std::string& what);
void check_equal(std::int64_t actual, std::int64_t expected, const std::string& what);
void section(const std::string& title);

std::filesystem::path scratch_dir();
void write_text_file(const std::filesystem::path& path, const std::string& content);

void test_value_parsing();
void test_json();
void test_csv_records();
void test_sniffer(const std::filesystem::path& directory);
void test_expressions();
void test_validation(const std::filesystem::path& directory);
void test_engine(const std::filesystem::path& directory);
void test_bad_rows(const std::filesystem::path& directory);
void test_partition_coverage(const std::filesystem::path& directory);
void test_web_guards(const std::filesystem::path& directory);
void test_ai_layer(const std::filesystem::path& directory);
void test_end_to_end(const std::filesystem::path& directory);

} // namespace merope
