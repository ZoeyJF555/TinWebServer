// Unit tests for the HTTP module. No framework: a check macro and an exit code.

#include <cstdio>
#include <string>

#include "http.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* expression, const char* file, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
    }
}

}  // namespace

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

namespace {

void test_incomplete_request() {
    HttpRequest request;
    CHECK(parse_request("GET / HTTP/1.1\r\nHost: localhost\r\n", request) ==
          ParseState::Incomplete);
}

void test_plain_request() {
    HttpRequest request;
    const auto state = parse_request(
        "GET /index.html HTTP/1.1\r\nHost: localhost\r\nAccept: */*\r\n\r\n", request);

    CHECK(state == ParseState::Done);
    CHECK(request.method == "GET");
    CHECK(request.target == "/index.html");
    CHECK(request.path == "/index.html");
    CHECK(request.version == "HTTP/1.1");
    CHECK(request.keep_alive);
    CHECK(request.headers.at("host") == "localhost");
    CHECK(request.headers.at("accept") == "*/*");
}

void test_query_string_is_split() {
    HttpRequest request;
    parse_request("GET /search?q=mu&page=2 HTTP/1.1\r\n\r\n", request);

    CHECK(request.path == "/search");
    CHECK(request.query == "q=mu&page=2");
}

void test_connection_close() {
    HttpRequest request;
    parse_request("GET / HTTP/1.1\r\nConnection: close\r\n\r\n", request);
    CHECK(!request.keep_alive);
}

void test_http_10_defaults_to_close() {
    HttpRequest request;
    parse_request("GET / HTTP/1.0\r\n\r\n", request);
    CHECK(!request.keep_alive);

    HttpRequest keep;
    parse_request("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", keep);
    CHECK(keep.keep_alive);
}

void test_malformed_requests() {
    HttpRequest request;
    CHECK(parse_request("GET /\r\n\r\n", request) == ParseState::Error);
    CHECK(parse_request("GET / HTTP/1.1\r\nBroken header\r\n\r\n", request) ==
          ParseState::Error);
    CHECK(parse_request("NOPROTOCOL\r\n\r\n", request) == ParseState::Error);
}

void test_url_decode() {
    CHECK(url_decode("/a%20b") == "/a b");
    CHECK(url_decode("/a+b") == "/a b");
    CHECK(url_decode("/%41%42") == "/AB");
    CHECK(url_decode("/plain") == "/plain");
    CHECK(url_decode("/%zz") == "/%zz");
}

void test_mime_types() {
    CHECK(mime_type("/index.html").rfind("text/html", 0) == 0);
    CHECK(mime_type("/style.css").rfind("text/css", 0) == 0);
    CHECK(mime_type("/app.js").rfind("text/javascript", 0) == 0);
    CHECK(mime_type("/data.json").rfind("application/json", 0) == 0);
    CHECK(mime_type("/logo.png") == "image/png");
    CHECK(mime_type("/no-extension") == "application/octet-stream");
    CHECK(mime_type("/x.unknownext") == "application/octet-stream");
}

void test_resolve_path() {
    std::string path;

    CHECK(resolve_path("www", "/", path));
    CHECK(path == "www/index.html");

    CHECK(resolve_path("www", "/style.css", path));
    CHECK(path == "www/style.css");

    CHECK(resolve_path("www", "/assets/app.js", path));
    CHECK(path == "www/assets/app.js");

    CHECK(resolve_path("www", "/docs/", path));
    CHECK(path == "www/docs/index.html");

    CHECK(resolve_path("www", "/style.css?v=2", path));
    CHECK(path == "www/style.css");
}

void test_traversal_is_rejected() {
    std::string path;

    CHECK(!resolve_path("www", "/../etc/passwd", path));
    CHECK(!resolve_path("www", "/a/../../b", path));
    CHECK(!resolve_path("www", "/%2e%2e/etc/passwd", path));
    CHECK(!resolve_path("www", "no-leading-slash", path));
}

}  // namespace

int main() {
    test_incomplete_request();
    test_plain_request();
    test_query_string_is_split();
    test_connection_close();
    test_http_10_defaults_to_close();
    test_malformed_requests();
    test_url_decode();
    test_mime_types();
    test_resolve_path();
    test_traversal_is_rejected();

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
