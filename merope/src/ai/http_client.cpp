#include "http_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <cstdlib>
#include <memory>

namespace {

// Every WinHTTP handle is closed by the same deleter, so an early return can
// never leak one. There is no naked close in this file.
struct winhttp_closer_t {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr) WinHttpCloseHandle(handle);
    }
};
using winhttp_handle_t = std::unique_ptr<void, winhttp_closer_t>;

std::wstring widen(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        wide.data(), needed);
    return wide;
}

std::string describe_last_error(const std::string& stage) {
    const DWORD code = GetLastError();
    std::string message = stage + " failed (WinHTTP error " + std::to_string(code) + ")";
    switch (code) {
    case ERROR_WINHTTP_CANNOT_CONNECT:      message += ": nothing accepted the connection"; break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:   message += ": the host name did not resolve"; break;
    case ERROR_WINHTTP_TIMEOUT:             message += ": timed out"; break;
    case ERROR_WINHTTP_SECURE_FAILURE:      message += ": the TLS certificate was not accepted"; break;
    case ERROR_WINHTTP_INVALID_URL:         message += ": the URL is not one WinHTTP understands"; break;
    default: break;
    }
    return message;
}

} // namespace

bool merope::split_url(const std::string& url, std::string& scheme, std::string& host,
                       std::uint16_t& port, std::string& path, std::string& error) {
    scheme.clear();
    host.clear();
    path.clear();
    port = 0;

    const std::size_t separator = url.find("://");
    if (separator == std::string::npos) {
        error = "a URL needs a scheme: " + url;
        return false;
    }
    scheme = url.substr(0, separator);
    if (scheme != "https" && scheme != "http") {
        error = "only http and https are supported, not " + scheme;
        return false;
    }

    const std::size_t host_begin = separator + 3;
    const std::size_t path_begin = url.find('/', host_begin);
    std::string       authority  = path_begin == std::string::npos
                                       ? url.substr(host_begin)
                                       : url.substr(host_begin, path_begin - host_begin);
    path = path_begin == std::string::npos ? "/" : url.substr(path_begin);

    if (authority.empty()) {
        error = "the URL has no host: " + url;
        return false;
    }

    port = scheme == "https" ? 443 : 80;
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        const std::string digits = authority.substr(colon + 1);
        // Rejected rather than ignored: a port that silently falls back to 443
        // sends the request somewhere the caller did not ask for.
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
            error = "the port in " + url + " is not a number";
            return false;
        }
        const unsigned long value = std::strtoul(digits.c_str(), nullptr, 10);
        if (value == 0 || value > 65535) {
            error = "the port in " + url + " is out of range";
            return false;
        }
        port      = static_cast<std::uint16_t>(value);
        authority = authority.substr(0, colon);
    }

    host = authority;
    return true;
}

merope::http_reply_t merope::http_send(const http_call_t& call) {
    http_reply_t reply;

    std::string scheme;
    std::string host;
    std::string path;
    std::uint16_t port = 0;
    if (!split_url(call.url, scheme, host, port, path, reply.error)) return reply;

    const winhttp_handle_t session(WinHttpOpen(L"merope/1.0",
                                               WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        reply.error = describe_last_error("opening an HTTP session");
        return reply;
    }

    const int milliseconds = (call.timeout_seconds > 0 ? call.timeout_seconds : 60) * 1000;
    WinHttpSetTimeouts(session.get(), milliseconds, milliseconds, milliseconds, milliseconds);

    const winhttp_handle_t connection(WinHttpConnect(session.get(), widen(host).c_str(),
                                                     static_cast<INTERNET_PORT>(port), 0));
    if (!connection) {
        reply.error = describe_last_error("connecting to " + host);
        return reply;
    }

    const DWORD flags = scheme == "https" ? WINHTTP_FLAG_SECURE : 0u;
    const winhttp_handle_t request(WinHttpOpenRequest(connection.get(), widen(call.method).c_str(),
                                                      widen(path).c_str(), nullptr,
                                                      WINHTTP_NO_REFERER,
                                                      WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) {
        reply.error = describe_last_error("preparing the request");
        return reply;
    }

    std::string header_block;
    for (const auto& entry : call.headers) {
        header_block += entry.first + ": " + entry.second + "\r\n";
    }
    const std::wstring headers = widen(header_block);

    const BOOL sent = WinHttpSendRequest(
        request.get(),
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0u : static_cast<DWORD>(-1),
        call.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(call.body.data()),
        static_cast<DWORD>(call.body.size()),
        static_cast<DWORD>(call.body.size()), 0);
    if (sent == FALSE) {
        reply.error = describe_last_error("sending the request to " + host);
        return reply;
    }

    if (WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
        reply.error = describe_last_error("waiting for a response from " + host);
        return reply;
    }

    DWORD status      = 0;
    DWORD status_size = sizeof(status);
    if (WinHttpQueryHeaders(request.get(),
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                            WINHTTP_NO_HEADER_INDEX) == FALSE) {
        reply.error = describe_last_error("reading the status line");
        return reply;
    }
    reply.status = static_cast<int>(status);

    // Read to the end whatever the status: an error body is the part that says
    // which key was rejected, or which field the model did not like.
    for (;;) {
        DWORD available = 0;
        if (WinHttpQueryDataAvailable(request.get(), &available) == FALSE) {
            reply.error = describe_last_error("reading the response body");
            return reply;
        }
        if (available == 0) break;

        const std::size_t offset = reply.body.size();
        reply.body.resize(offset + available);
        DWORD read = 0;
        if (WinHttpReadData(request.get(), reply.body.data() + offset, available, &read) == FALSE) {
            reply.error = describe_last_error("reading the response body");
            return reply;
        }
        reply.body.resize(offset + read);
        if (read == 0) break;
    }

    reply.completed = true;
    return reply;
}
