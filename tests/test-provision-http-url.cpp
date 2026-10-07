// provision-admin: common_http_parse_url() takes the user info only from the authority of a URL.
//
// It used to take everything before the first '@' anywhere after "://" as the user info. The Hugging Face
// download builds its URLs from repository file names without encoding them (common/hf-cache.cpp), so a file
// named like "x@evil.example/y.gguf" moved the request - with its Authorization: Bearer <HF_TOKEN> header -
// to another host.

#include "http.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int n_failed = 0;

#define CHECK_EQ(actual, expected) do {                                                          \
    const auto a_ = (actual);                                                                    \
    const auto e_ = (expected);                                                                  \
    if (!(a_ == e_)) {                                                                           \
        std::fprintf(stderr, "%s:%d: CHECK_EQ(%s, %s) failed\n", __FILE__, __LINE__, #actual, #expected); \
        n_failed++;                                                                              \
    }                                                                                            \
} while (0)

static void check_url(const std::string & url, const std::string & user, const std::string & password,
                      const std::string & host, int port, const std::string & path) {
    const common_http_url u = common_http_parse_url(url);
    if (u.user != user || u.password != password || u.host != host || u.port != port || u.path != path) {
        std::fprintf(stderr, "FAIL %s\n  got user='%s' password='%s' host='%s' port=%d path='%s'\n", url.c_str(),
                     u.user.c_str(), u.password.c_str(), u.host.c_str(), u.port, u.path.c_str());
        n_failed++;
    }
}

int main() {
    // user info in the authority keeps working
    check_url("https://user:pass@example.com/a/b", "user", "pass", "example.com", 443, "/a/b");
    check_url("http://user@example.com:8080/", "user", "", "example.com", 8080, "/");
    check_url("http://u:p@[::1]:9000/x", "u", "p", "::1", 9000, "/x");
    // the last '@' of the authority ends the user info (WHATWG / browsers)
    check_url("https://a@b@example.com/x", "a@b", "", "example.com", 443, "/x");

    // a '@' in the path, the query or the fragment is not user info: the host stays the same
    check_url("https://huggingface.co/org/repo/resolve/main/x@evil.example/y.gguf", "", "", "huggingface.co", 443,
              "/org/repo/resolve/main/x@evil.example/y.gguf");
    check_url("http://127.0.0.1:8123/org/repo/resolve/main/x@127.0.0.1:9/y.gguf", "", "", "127.0.0.1", 8123,
              "/org/repo/resolve/main/x@127.0.0.1:9/y.gguf");
    check_url("https://example.com/a?next=user@evil.example", "", "", "example.com", 443, "/a?next=user@evil.example");
    // without a path the host is never taken from after a '@' of the query or the fragment
    for (const char * url : { "https://example.com?u=x@evil.example", "https://example.com#x@evil.example" }) {
        const common_http_url u = common_http_parse_url(url);
        if (!u.user.empty() || u.host == "evil.example") {
            std::fprintf(stderr, "FAIL %s: user='%s' host='%s'\n", url, u.user.c_str(), u.host.c_str());
            n_failed++;
        }
    }

    // the masked form never shows a password, and has none for a URL without user info
    CHECK_EQ(common_http_show_masked_url(common_http_parse_url("https://u:p@example.com/a")), std::string("https://****:****@example.com/a"));
    CHECK_EQ(common_http_show_masked_url(common_http_parse_url("https://example.com/x@y")), std::string("https://example.com/x@y"));

    if (n_failed) {
        std::fprintf(stderr, "%d check(s) failed\n", n_failed);
        return 1;
    }
    std::printf("all URL checks passed\n");
    return 0;
}
