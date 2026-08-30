// web/http_server.h - a small HTTP/1.1 server (spec 7).
//
// The project takes no third party dependencies, so instead of Crow or Drogon
// this is a few hundred lines over Winsock, served by the projects own thread
// pool. It binds to the loopback interface only: the API can open files by
// path, and that is a capability that has no business on a network socket.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace merope {

struct http_request_t {
    std::string method;
    std::string target;   // as received, e.g. /api/query?x=1
    std::string path;     // decoded, without the query string
    std::string query;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    std::string header(std::string_view name) const;
    std::string query_param(std::string_view name) const;
};

struct http_response_t {
    int         status       = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    void json(std::string payload);
    void text(int code, std::string message);
};

using http_handler_t = std::function<void(const http_request_t&, http_response_t&)>;

class c_http_server {
public:
    explicit c_http_server(std::uint16_t port, std::size_t workers = 0);
    ~c_http_server();

    c_http_server(const c_http_server&)            = delete;
    c_http_server& operator=(const c_http_server&) = delete;

    void route(std::string method, std::string path, http_handler_t handler);
    void set_fallback(http_handler_t handler);

    // Binds and listens. Returns false with a reason rather than throwing, so
    // the caller can suggest another port.
    bool start(std::string& error);

    // Accept loop. Blocks until stop() is called from another thread.
    void run();

    // Closes the listener, which wakes the blocked accept() and ends run().
    // Safe to call from a request handler: the connection being served owns its
    // own socket, so the reply still goes out before run() returns.
    void stop();

    bool running() const noexcept { return m_running.load(std::memory_order_relaxed); }

    std::uint16_t port() const noexcept { return m_port; }

private:
    void handle_connection(std::uintptr_t socket) const;
    void dispatch(const http_request_t& request, http_response_t& response) const;

    std::uint16_t  m_port;
    std::size_t    m_workers;
    std::uintptr_t m_listener = static_cast<std::uintptr_t>(~0ull);
    // Written by whichever thread calls stop(), read by the accept loop.
    std::atomic<bool> m_running{false};

    struct route_t {
        std::string    method;
        std::string    path;
        http_handler_t handler;
    };
    std::vector<route_t> m_routes;
    http_handler_t       m_fallback;
};

// Percent-decoding for path segments and form values.
std::string url_decode(std::string_view text);

} // namespace merope
