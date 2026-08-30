// ai/http_client.h - the only place in the process that opens a connection to
// something that is not this machine.
//
// The web server is Winsock and plaintext because it binds to loopback; a model
// endpoint is neither, so this is WinHTTP, which brings TLS, the system proxy
// and the certificate store with it. Like Winsock, it ships with Windows: the
// project still has no third party dependency.
//
// Nothing here knows what a model is. It sends bytes and reports what came
// back, including the failures, because a request that fails silently against a
// paid API is worse than one that fails loudly.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace merope {

struct http_call_t {
    std::string method = "POST";
    std::string url;    // https://host[:port]/path
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    int         timeout_seconds = 60;
};

struct http_reply_t {
    // True when the exchange completed. A 4xx is a completed exchange: the
    // status is the answer, not a failure of the transport.
    bool        completed = false;
    int         status    = 0;
    std::string body;
    std::string error;   // transport level failure, empty when completed

    bool ok() const noexcept { return completed && status >= 200 && status < 300; }
};

http_reply_t http_send(const http_call_t& call);

// Splits a URL into its parts. Exposed because getting this wrong sends a
// request, with a key attached, to a host nobody intended.
bool split_url(const std::string& url, std::string& scheme, std::string& host,
               std::uint16_t& port, std::string& path, std::string& error);

} // namespace merope
