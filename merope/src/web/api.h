#pragma once

#include "../ai/remote_ai_provider.h"
#include "http_server.h"

#include <cstdint>
#include <string>

namespace merope {

struct web_options_t {
    std::string data_root;
    bool        allow_generate = true;
    // Whether the browser may stop the process. The kill button is the only
    // route that acts on the server itself, so it can be turned off entirely.
    bool        allow_shutdown = true;
    // Which model answers, resolved once at startup. The key lives here and
    // nowhere else: it is never put in a response, and never in the page.
    ai_resolution_t ai;
    std::size_t sample_rows    = 10000;
    std::uint64_t seed         = 20260101;
};

void register_api(c_http_server& server, const web_options_t& options);

ai_settings_t settings_for_request(const web_options_t& options, const std::string& model);

std::string make_session_token();

// Compares the presented token against the session one. An empty or
// mismatched token is refused; exposed so the guard can be tested directly.
bool shutdown_token_matches(const std::string& expected, const std::string& presented);

bool resolve_dataset_path(const std::string& root, const std::string& requested,
                          std::string& resolved, std::string& error);

}
