// web/api.h - the REST surface the browser talks to (spec 7).
//
// Every endpoint goes through app/pipeline.h, exactly like the CLI does. The
// web layer adds no processing of its own; if it did, the two front ends would
// drift apart.
#pragma once

#include "../ai/remote_ai_provider.h"
#include "http_server.h"

#include <cstdint>
#include <string>

namespace merope {

struct web_options_t {
    // Datasets may only be opened from inside this directory. The API takes
    // file paths, so without a root a browser tab could read anything the user
    // can read.
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

// The settings a single request should use: the configured ones, with the model
// swapped when the page asked for a different one. An unknown provider or an
// empty model falls back to what was configured rather than failing the request.
ai_settings_t settings_for_request(const web_options_t& options, const std::string& model);

// A random per process token. The page is served from this origin and can read
// it back from /api/session; a page on another site cannot, because nothing
// here sends CORS headers. /api/shutdown will not fire without it.
std::string make_session_token();

// Compares the presented token against the session one. An empty or
// mismatched token is refused; exposed so the guard can be tested directly.
bool shutdown_token_matches(const std::string& expected, const std::string& presented);

// Resolves a dataset path from a request against the served root, refusing
// anything that escapes it. Exposed so the guard can be tested directly: it is
// the only thing standing between a browser tab and the rest of the disk.
bool resolve_dataset_path(const std::string& root, const std::string& requested,
                          std::string& resolved, std::string& error);

} // namespace merope
