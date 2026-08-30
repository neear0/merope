// app/cli.h - the command line front end.
//
// Every command goes through app/pipeline.h, the same seam the web layer uses,
// so the two front ends cannot drift apart.
#pragma once

#include "../ai/remote_ai_provider.h"
#include "../dataset/csv_reader.h"
#include "../dataset/sampler.h"
#include "../engine/processing_engine.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace merope {

struct cli_arguments_t {
    std::string              command;
    std::vector<std::string> positional;

    std::size_t      workers          = 0;
    std::size_t      partitions       = 0;
    std::size_t      chunk_rows       = k_default_chunk_rows;
    std::size_t      sample_rows      = 10000;
    std::uint64_t    seed             = 20260101;
    bad_row_policy_t policy           = bad_row_policy_t::skip;
    bool             measure_baseline = false;
    bool             use_cache        = true;
    bool             confirm          = false;
    bool             no_ai            = false;
    bool             show_prompt      = false;
    bool             write_header     = true;
    std::uint64_t    rows             = 100000;
    double           corrupt_fraction = 0.0;

    std::uint16_t    port             = 7433;
    std::string      data_dir;
    bool             open_browser     = true;
    // The web UI can stop the process it is served by. --no-kill takes that
    // button away for a server meant to outlive the tab.
    bool             allow_kill       = true;

    // Which model answers. The key is deliberately not among these: a command
    // line ends up in shell history, so it comes from the environment or from
    // the config file instead.
    std::string      ai_provider;
    std::string      ai_model;
    std::string      ai_base;
    std::string      ai_config;
    int              ai_timeout       = 0;

    // Benchmark suite (spec chapter 12).
    std::string      bench_output;
    std::string      bench_suites;
    std::string      scale_sizes;
    std::string      thread_list;
    std::size_t      repeats          = 3;
    bool             keep_datasets    = false;
};

void print_usage();

// Returns false when the arguments do not name a runnable command, which the
// caller turns into the usage text.
bool parse_arguments(int argc, char** argv, cli_arguments_t& out);

sample_options_t    sample_options_from(const cli_arguments_t& arguments);

// Resolves which provider and model this invocation should use, from the flags
// above, then the environment, then the config file. Never fails: the mock is
// always a legitimate answer, and it says so.
ai_resolution_t     resolve_ai_from(const cli_arguments_t& arguments);

// One line naming the provider, the model and where that decision came from.
// Never contains the key.
std::string         describe_ai(const ai_resolution_t& resolution);
execution_options_t execution_options_from(const cli_arguments_t& arguments);

int command_profile(const cli_arguments_t& arguments);
int command_schema(const cli_arguments_t& arguments);
int command_query(const cli_arguments_t& arguments);
int command_plan(const cli_arguments_t& arguments);
int command_generate(const cli_arguments_t& arguments);
int command_serve(const cli_arguments_t& arguments);
int command_bench(const cli_arguments_t& arguments);
int command_models(const cli_arguments_t& arguments);

int run_self_tests(std::ostream& out);

} // namespace merope
