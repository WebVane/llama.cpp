// HTTP transport abstraction for the license activation client.
//
// Single implementation: Http.cpp uses cpp-httplib, which provision-server
// already links (it is the HTTP server framework), so the activation
// transport is available unconditionally.  TLS for https:// activation URLs
// needs CPPHTTPLIB_OPENSSL_SUPPORT (LLAMA_OPENSSL=ON or LLAMA_BUILD_BORINGSSL=ON).
//
// The interface is deliberately function-shaped so tests can drop in
// std::function-based mocks via
// ActivationClient::set_transport(...) without inheritance ceremony.
//
// The call is a plain blocking client request on the calling thread: no
// event loop and no threading constraints.
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <string>

namespace visio::license {

struct HttpRequest {
    std::string method;        // "POST" / "GET"
    std::string url;
    std::map<std::string, std::string> headers;  // case-insensitive on server side, we send as-given
    std::string body;
    std::chrono::milliseconds timeout{15000};
};

struct HttpResponse {
    // HTTP status code on a completed request (1xx/2xx/3xx/4xx/5xx).
    // 0 when the transport never got that far (DNS, connect, TLS,
    // timeout) — in which case `transport_error` carries the diagnostic.
    int code = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string transport_error;  // populated on code==0
};

using HttpTransport = std::function<HttpResponse(const HttpRequest&)>;

// cpp-httplib-backed transport.  Always linked; safe to call from any thread.
HttpTransport default_http_transport();

}  // namespace visio::license
