#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

struct HttpRequest {
    std::string method;
    std::string target;
    std::string version;
    std::string path;
    std::string query;
    std::unordered_map<std::string, std::string> headers;

    bool keep_alive = true;
};

enum class ParseState { Incomplete, Done, Error };

// Parses the request head (everything up to and including the blank line).
// Returns Incomplete while the head is not complete yet.
ParseState parse_request(std::string_view buffer, HttpRequest& request);

// Decodes %XX escapes and '+' in the target.
std::string url_decode(std::string_view text);

// Maps a file extension to a MIME type.
std::string mime_type(std::string_view path);

// Turns a request target into a path inside `root`. Returns false when the
// target is malformed or tries to escape the document root.
bool resolve_path(const std::string& root, std::string_view target, std::string& out);
