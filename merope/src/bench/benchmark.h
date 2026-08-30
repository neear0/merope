// bench/benchmark.h - the experimental part of the specification (chapter 12).
//
// Five suites, each writing one CSV plus a shared manifest recording the seed,
// the provider, the build and the machine, because a measurement nobody can
// reproduce is not a measurement.
//
//   inference  12.1  heuristics vs heuristics+AI, and the effect of sampling method
//   sampling   12.2  sample size 100 .. 100k, and how stable the estimates get
//   threads    12.3  1 .. 32 workers: time, speedup, efficiency, throughput
//   scaling    12.4  1 .. 50 GB: does peak memory stay flat as the file grows
//   baseline   12.5  the same workload on DuckDB or Polars
#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace merope {

struct bench_options_t {
    std::string   output_dir;      // where result CSVs land
    std::string   work_dir;        // where generated datasets land
    std::uint64_t seed          = 20260101;
    std::size_t   repeats       = 3;
    std::string   dataset;         // reuse this file instead of generating one
    bool          keep_datasets = false;

    std::uint64_t inference_rows = 2000000;
    std::uint64_t threads_rows   = 8000000;

    std::vector<std::size_t> thread_counts;
    std::vector<std::size_t> sample_sizes;
    std::vector<double>      scale_gigabytes;
};

struct machine_info_t {
    std::string   cpu;
    std::size_t   logical_cores  = 0;
    std::uint64_t memory_bytes   = 0;
    std::string   os;
};

machine_info_t describe_machine();
std::string    describe_build();

// Runs the named suites (or all of them) and writes the results under
// options.output_dir. Returns 0 on success.
int run_benchmarks(const bench_options_t& options, const std::vector<std::string>& suites,
                   std::ostream& log);

} // namespace merope
