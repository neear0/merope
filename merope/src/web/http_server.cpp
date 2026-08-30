#include "http_server.h"

#include "../parallel/thread_pool.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

static constexpr std::size_t k_max_header_bytes = 64 * 1024;
static constexpr std::size_t k_max_body_bytes   = 16 * 1024 * 1024;
static constexpr std::uintptr_t k_invalid_socket = static_cast<std::uintptr_t>(INVALID_SOCKET);

// Winsock has to be started once per process. The teardown is registered with
// atexit rather than held in a guard object, so no class is needed here.
static bool winsock_ready() {
    static const bool ready = [] {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
        std::atexit([] { WSACleanup(); });
        return true;
    }();
    return ready;
}

static bool equals_ignore_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

static std::string_view trim_view(std::string_view text) noexcept {
    std::size_t begin = 0;
    std::size_t end   = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
    return text.substr(begin, end - begin);
}

static const char* status_text(int status) noexcept {
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default:  return "OK";
    }
}

static bool send_all(SOCKET socket, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const int chunk = static_cast<int>(std::min<std::size_t>(size - sent, 1 << 16));
        const int wrote = send(socket, data + sent, chunk, 0);
        if (wrote <= 0) return false;
        sent += static_cast<std::size_t>(wrote);
    }
    return true;
}

static bool parse_request(const std::string& head, const std::string& body, merope::http_request_t& out) {
    std::size_t line_end = head.find("\r\n");
    if (line_end == std::string::npos) return false;

    const std::string_view request_line(head.data(), line_end);
    const std::size_t      first_space = request_line.find(' ');
    if (first_space == std::string_view::npos) return false;
    const std::size_t second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) return false;

    out.method.assign(request_line.substr(0, first_space));
    out.target.assign(request_line.substr(first_space + 1, second_space - first_space - 1));

    const std::size_t question = out.target.find('?');
    if (question == std::string::npos) {
        out.path  = merope::url_decode(out.target);
        out.query.clear();
    } else {
        out.path  = merope::url_decode(std::string_view(out.target).substr(0, question));
        out.query = out.target.substr(question + 1);
    }

    std::size_t cursor = line_end + 2;
    while (cursor < head.size()) {
        const std::size_t end = head.find("\r\n", cursor);
        if (end == std::string::npos || end == cursor) break;
        const std::string_view line(head.data() + cursor, end - cursor);
        const std::size_t      colon = line.find(':');
        if (colon != std::string_view::npos) {
            out.headers.emplace_back(std::string(trim_view(line.substr(0, colon))),
                                     std::string(trim_view(line.substr(colon + 1))));
        }
        cursor = end + 2;
    }

    out.body = body;
    return true;
}

std::string merope::url_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char ch = text[index];
        if (ch == '+') {
            out.push_back(' ');
            continue;
        }
        if (ch == '%' && index + 2 < text.size()) {
            auto hex = [](char digit) -> int {
                if (digit >= '0' && digit <= '9') return digit - '0';
                if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
                if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
                return -1;
            };
            const int high = hex(text[index + 1]);
            const int low  = hex(text[index + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>(high * 16 + low));
                index += 2;
                continue;
            }
        }
        out.push_back(ch);
    }
    return out;
}

std::string merope::http_request_t::header(std::string_view name) const {
    for (const auto& entry : headers) {
        if (equals_ignore_case(entry.first, name)) return entry.second;
    }
    return std::string();
}

std::string merope::http_request_t::query_param(std::string_view name) const {
    std::size_t cursor = 0;
    while (cursor <= query.size()) {
        const std::size_t      end  = std::min(query.find('&', cursor), query.size());
        const std::string_view pair(query.data() + cursor, end - cursor);
        const std::size_t      equals = pair.find('=');
        if (equals != std::string_view::npos && pair.substr(0, equals) == name) {
            return url_decode(pair.substr(equals + 1));
        }
        if (end == query.size()) break;
        cursor = end + 1;
    }
    return std::string();
}

void merope::http_response_t::json(std::string payload) {
    // Deliberately does not touch `status`: a refusal is JSON too, and it has
    // to keep saying 4xx rather than reporting success with an error inside.
    content_type = "application/json; charset=utf-8";
    body         = std::move(payload);
}

void merope::http_response_t::text(int code, std::string message) {
    status       = code;
    content_type = "text/plain; charset=utf-8";
    body         = std::move(message);
}

merope::c_http_server::c_http_server(std::uint16_t port, std::size_t workers)
    : m_port(port), m_workers(workers == 0 ? default_worker_count() : workers) {}

merope::c_http_server::~c_http_server() {
    if (m_listener != k_invalid_socket) {
        closesocket(static_cast<SOCKET>(m_listener));
        m_listener = k_invalid_socket;
    }
}

void merope::c_http_server::route(std::string method, std::string path, http_handler_t handler) {
    m_routes.push_back(route_t{std::move(method), std::move(path), std::move(handler)});
}

void merope::c_http_server::set_fallback(http_handler_t handler) {
    m_fallback = std::move(handler);
}

bool merope::c_http_server::start(std::string& error) {
    if (!winsock_ready()) {
        error = "could not initialise Winsock";
        return false;
    }

    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        error = "could not create a socket";
        return false;
    }

    // Loopback only. The API can read files by path, so exposing it beyond this
    // machine would turn a local tool into a remote file reader.
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_port        = htons(m_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(listener);
        error = "port " + std::to_string(m_port) + " is already in use";
        return false;
    }
    if (listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listener);
        error = "could not listen on port " + std::to_string(m_port);
        return false;
    }

    // Port 0 means "any free port"; report back which one we actually got.
    sockaddr_in bound{};
    int         bound_size = sizeof(bound);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_size) == 0) {
        m_port = ntohs(bound.sin_port);
    }

    m_listener = static_cast<std::uintptr_t>(listener);
    m_running.store(true, std::memory_order_relaxed);
    return true;
}

void merope::c_http_server::run() {
    if (m_listener == k_invalid_socket) return;

    c_thread_pool pool(m_workers);
    while (m_running.load(std::memory_order_relaxed)) {
        const SOCKET client = accept(static_cast<SOCKET>(m_listener), nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!m_running.load(std::memory_order_relaxed)) break;
            continue;
        }
        const std::uintptr_t handle = static_cast<std::uintptr_t>(client);
        pool.submit([this, handle] { handle_connection(handle); });
    }
    pool.wait_for_all();
}

void merope::c_http_server::stop() {
    m_running.store(false, std::memory_order_relaxed);
    if (m_listener != k_invalid_socket) {
        closesocket(static_cast<SOCKET>(m_listener));
        m_listener = k_invalid_socket;
    }
}

void merope::c_http_server::dispatch(const http_request_t& request, http_response_t& response) const {
    bool path_exists = false;
    for (const route_t& entry : m_routes) {
        if (entry.path != request.path) continue;
        path_exists = true;
        if (entry.method != request.method) continue;
        entry.handler(request, response);
        return;
    }
    if (path_exists) {
        response.text(405, "method not allowed");
        return;
    }
    if (m_fallback) {
        m_fallback(request, response);
        return;
    }
    response.text(404, "not found");
}

void merope::c_http_server::handle_connection(std::uintptr_t handle) const {
    const SOCKET client = static_cast<SOCKET>(handle);

    std::string buffer;
    char        scratch[16384];
    std::size_t header_end = std::string::npos;

    // Read until the blank line that ends the header block.
    while (header_end == std::string::npos) {
        const int got = recv(client, scratch, static_cast<int>(sizeof(scratch)), 0);
        if (got <= 0) {
            closesocket(client);
            return;
        }
        buffer.append(scratch, static_cast<std::size_t>(got));
        header_end = buffer.find("\r\n\r\n");
        if (header_end == std::string::npos && buffer.size() > k_max_header_bytes) {
            closesocket(client);
            return;
        }
    }

    const std::string head = buffer.substr(0, header_end + 2);

    // Content-Length decides how much body still has to arrive.
    std::size_t content_length = 0;
    {
        http_request_t probe;
        parse_request(head, std::string(), probe);
        const std::string value = probe.header("Content-Length");
        if (!value.empty()) content_length = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
    }
    if (content_length > k_max_body_bytes) {
        const std::string reply =
            "HTTP/1.1 413 Payload Too Large\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        send_all(client, reply.data(), reply.size());
        closesocket(client);
        return;
    }

    std::string body = buffer.substr(header_end + 4);
    while (body.size() < content_length) {
        const int got = recv(client, scratch, static_cast<int>(sizeof(scratch)), 0);
        if (got <= 0) break;
        body.append(scratch, static_cast<std::size_t>(got));
    }
    body.resize(std::min(body.size(), content_length));

    http_request_t request;
    http_response_t response;
    if (!parse_request(head, body, request)) {
        response.text(400, "malformed request");
    } else {
        try {
            dispatch(request, response);
        } catch (const std::exception& error) {
            response.text(500, std::string("error: ") + error.what());
        }
    }

    std::string reply = "HTTP/1.1 " + std::to_string(response.status) + " " +
                        status_text(response.status) + "\r\n";
    reply += "Content-Type: " + response.content_type + "\r\n";
    reply += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    reply += "Connection: close\r\n";
    // The UI is served from this same origin, so nothing else needs access.
    reply += "X-Content-Type-Options: nosniff\r\n";
    for (const auto& entry : response.headers) {
        reply += entry.first + ": " + entry.second + "\r\n";
    }
    reply += "\r\n";
    reply += response.body;

    send_all(client, reply.data(), reply.size());
    shutdown(client, SD_SEND);
    closesocket(client);
}

