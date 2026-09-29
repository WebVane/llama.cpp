// cpp-httplib-backed activation transport.
//
// Why cpp-httplib and not drogon / libcurl?  provision-server already links
// cpp-httplib (it is the HTTP server framework), so reusing it for the one
// outbound call the license client makes keeps the dependency set unchanged.
// TLS for https:// activation URLs is available whenever the build has
// CPPHTTPLIB_OPENSSL_SUPPORT, i.e. LLAMA_OPENSSL=ON (system OpenSSL) or
// LLAMA_BUILD_BORINGSSL=ON (bundled) — the same switch that gives the server
// its own HTTPS support.  Without it an https:// URL fails loud instead of
// silently downgrading.
//
// The call is synchronous: ActivationClient's API returns the result on the
// stack, and activation runs at most a handful of times per process.
#include "license/Http.h"

#include <cpp-httplib/httplib.h>

#include <cctype>
#include <string>
#include <string_view>

namespace visio::license {
namespace {

// Split a full URL ("http://host:port/api/v1/activate") into the base
// httplib::Client expects ("http://host:port") and the request path.
struct ParsedUrl {
    std::string base;
    std::string path;
    bool        is_https = false;
    bool        ok = false;
};

ParsedUrl parse_url(std::string_view url) {
    ParsedUrl out;
    constexpr std::string_view kHttps = "https://";
    constexpr std::string_view kHttp  = "http://";
    std::size_t host_start = 0;
    if (url.substr(0, kHttps.size()) == kHttps) {
        out.is_https = true;
        host_start = kHttps.size();
    } else if (url.substr(0, kHttp.size()) == kHttp) {
        out.is_https = false;
        host_start = kHttp.size();
    } else {
        return out;  // ok=false
    }
    const std::size_t path_start = url.find('/', host_start);
    if (path_start == std::string_view::npos) {
        out.base = std::string{url};
        out.path = "/";
    } else {
        out.base = std::string{url.substr(0, path_start)};
        out.path = std::string{url.substr(path_start)};
    }
    if (out.base.size() == host_start) {
        return out;  // "https://" with no host
    }
    out.ok = true;
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

HttpResponse perform_httplib(const HttpRequest& req) {
    HttpResponse out;

    const auto parsed = parse_url(req.url);
    if (!parsed.ok) {
        out.transport_error = "malformed URL: " + req.url +
            " (expected http://host[:port][/path] or https://...)";
        return out;
    }
#if !defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    if (parsed.is_https) {
        out.transport_error =
            "URL is https but this build has no TLS support — build with "
            "LLAMA_OPENSSL=ON or LLAMA_BUILD_BORINGSSL=ON";
        return out;
    }
#endif

    httplib::Client cli(parsed.base);
    if (!cli.is_valid()) {
        out.transport_error = "failed to create HTTP client for " + parsed.base;
        return out;
    }
    const auto sec  = static_cast<time_t>(req.timeout.count() / 1000);
    const auto usec = static_cast<time_t>((req.timeout.count() % 1000) * 1000);
    cli.set_connection_timeout(sec, usec);
    cli.set_read_timeout(sec, usec);
    cli.set_write_timeout(sec, usec);
    cli.set_follow_location(false);
#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    cli.enable_server_certificate_verification(true);
#endif

    // Content-Type travels as a dedicated argument in httplib's Post/Put API;
    // every other header (Idempotency-Key, Authorization, ...) is passed as-is.
    httplib::Headers headers;
    std::string content_type = "application/json";
    for (const auto& [k, v] : req.headers) {
        if (iequals(k, "Content-Type")) {
            content_type = v;
            continue;
        }
        headers.emplace(k, v);
    }

    httplib::Result result;
    if (req.method == "GET") {
        result = cli.Get(parsed.path, headers);
    } else if (req.method == "POST") {
        result = cli.Post(parsed.path, headers, req.body, content_type);
    } else if (req.method == "PUT") {
        result = cli.Put(parsed.path, headers, req.body, content_type);
    } else if (req.method == "DELETE") {
        result = cli.Delete(parsed.path, headers, req.body, content_type);
    } else {
        out.transport_error = "unsupported HTTP method: " + req.method;
        return out;
    }

    if (!result) {
        const auto err = result.error();
        out.transport_error = httplib::to_string(err);
        // The most common misconfiguration deserves a specific hint: an
        // https:// activation URL against a server whose certificate the
        // build's CA store does not trust.  No silent http downgrade.
        if (parsed.is_https &&
            (err == httplib::Error::SSLConnection ||
             err == httplib::Error::SSLServerVerification ||
             err == httplib::Error::SSLLoadingCerts)) {
            out.transport_error +=
                " (TLS failure on an https URL — check the License Server "
                "certificate chain / the system CA store)";
        }
        return out;
    }

    out.code = result->status;
    out.body = result->body;
    for (const auto& h : result->headers) {
        out.headers[h.first] = h.second;
    }
    return out;
}

}  // namespace

HttpTransport default_http_transport() {
    return perform_httplib;
}

}  // namespace visio::license
