#include "http.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace {

std::string to_lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

}  // namespace

ParseState parse_request(std::string_view buffer, HttpRequest& request) {
    const auto head_end = buffer.find("\r\n\r\n");
    if (head_end == std::string_view::npos) {
        return ParseState::Incomplete;
    }

    request = HttpRequest{};
    const std::string_view head = buffer.substr(0, head_end);

    const auto line_end = head.find("\r\n");
    const std::string_view request_line = head.substr(0, line_end);

    // METHOD SP TARGET SP VERSION
    const auto first_space = request_line.find(' ');
    if (first_space == std::string_view::npos) {
        return ParseState::Error;
    }
    const auto second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) {
        return ParseState::Error;
    }

    request.method = std::string(request_line.substr(0, first_space));
    request.target = std::string(
        request_line.substr(first_space + 1, second_space - first_space - 1));
    request.version = std::string(request_line.substr(second_space + 1));

    if (request.method.empty() || request.target.empty() ||
        request.version.rfind("HTTP/", 0) != 0) {
        return ParseState::Error;
    }

    std::size_t position = (line_end == std::string_view::npos) ? head.size() : line_end + 2;
    while (position < head.size()) {
        auto next = head.find("\r\n", position);
        if (next == std::string_view::npos) {
            next = head.size();
        }

        const std::string_view line = head.substr(position, next - position);
        position = next + 2;

        if (line.empty()) {
            continue;
        }

        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            return ParseState::Error;
        }

        const std::string name = to_lower(trim(line.substr(0, colon)));
        const std::string value = std::string(trim(line.substr(colon + 1)));
        request.headers[name] = value;
    }

    // Split the target into path and query string.
    const auto question = request.target.find('?');
    if (question == std::string::npos) {
        request.path = request.target;
    } else {
        request.path = request.target.substr(0, question);
        request.query = request.target.substr(question + 1);
    }

    const auto connection = request.headers.find("connection");
    const bool wants_close = connection != request.headers.end() &&
                             to_lower(connection->second) == "close";

    if (request.version == "HTTP/1.0") {
        // HTTP/1.0 closes by default; the client has to ask for keep-alive.
        request.keep_alive = connection != request.headers.end() &&
                             to_lower(connection->second) == "keep-alive";
    } else {
        request.keep_alive = !wants_close;
    }

    return ParseState::Done;
}

std::string url_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out.push_back(' ');
            continue;
        }

        if (text[i] == '%' && i + 2 < text.size() &&
            std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            const std::string hex{text[i + 1], text[i + 2]};
            out.push_back(static_cast<char>(std::stoi(hex, nullptr, 16)));
            i += 2;
            continue;
        }

        out.push_back(text[i]);
    }

    return out;
}

std::string mime_type(std::string_view path) {
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos) {
        return "application/octet-stream";
    }

    const std::string extension = to_lower(path.substr(dot + 1));
    if (extension == "html" || extension == "htm") return "text/html; charset=utf-8";
    if (extension == "css") return "text/css; charset=utf-8";
    if (extension == "js") return "text/javascript; charset=utf-8";
    if (extension == "json") return "application/json; charset=utf-8";
    if (extension == "txt") return "text/plain; charset=utf-8";
    if (extension == "svg") return "image/svg+xml";
    if (extension == "png") return "image/png";
    if (extension == "jpg" || extension == "jpeg") return "image/jpeg";
    if (extension == "gif") return "image/gif";
    if (extension == "ico") return "image/x-icon";
    if (extension == "woff2") return "font/woff2";

    return "application/octet-stream";
}

bool resolve_path(const std::string& root, std::string_view target,
                  std::string& out) {
    const auto question = target.find('?');
    const std::string_view raw =
        (question == std::string_view::npos) ? target : target.substr(0, question);

    const std::string decoded = url_decode(raw);
    if (decoded.empty() || decoded.front() != '/' ||
        decoded.find('\0') != std::string::npos) {
        return false;
    }

    std::vector<std::string> parts;
    std::size_t position = 0;
    while (position < decoded.size()) {
        const auto next = decoded.find('/', position);
        const std::size_t end = (next == std::string::npos) ? decoded.size() : next;
        const std::string part = decoded.substr(position, end - position);

        if (part == "..") {
            return false;  // Never leave the document root.
        }
        if (!part.empty() && part != ".") {
            parts.push_back(part);
        }

        if (next == std::string::npos) {
            break;
        }
        position = next + 1;
    }

    const bool directory = ends_with(decoded, "/");
    if (directory || parts.empty()) {
        parts.push_back("index.html");
    }

    std::string path = root;
    for (const auto& part : parts) {
        path.push_back('/');
        path.append(part);
    }

    out = std::move(path);
    return true;
}
