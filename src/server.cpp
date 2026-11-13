#include "server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http.h"

namespace {

constexpr int kMaxConnections = 1024;
constexpr std::size_t kMaxRequestSize = 64 * 1024;
constexpr int kMaxEvents = 64;

std::string status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 431: return "Request Header Fields Too Large";
        default: return "Internal Server Error";
    }
}

std::string http_date() {
    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);

    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &utc);
    return buffer;
}

std::string build_response(int code, const std::string& content_type,
                           const std::string& body, bool keep_alive,
                           bool head_only) {
    std::ostringstream out;
    out << "HTTP/1.1 " << code << ' ' << status_text(code) << "\r\n";
    out << "Server: tinweb\r\n";
    out << "Date: " << http_date() << "\r\n";
    out << "Content-Type: " << content_type << "\r\n";
    out << "Content-Length: " << body.size() << "\r\n";
    out << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";
    out << "\r\n";

    if (!head_only) {
        out << body;
    }

    return out.str();
}

bool read_file(const std::string& path, std::string& content) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    content = buffer.str();
    return true;
}

void make_non_blocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

std::string log_line(const std::string& method, const std::string& target,
                     int status) {
    std::ostringstream out;
    out << method << ' ' << target << " -> " << status;
    return out.str();
}

}  // namespace

Server::Server(std::string root, int port)
    : port_(port), root_(std::move(root)) {}

Server::~Server() {
    for (auto& entry : connections_) {
        ::close(entry.first);
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
    }
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

bool Server::listen_and_serve() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        std::perror("socket");
        return false;
    }

    const int reuse = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<std::uint16_t>(port_));

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        std::perror("bind");
        return false;
    }

    if (::listen(listen_fd_, 512) < 0) {
        std::perror("listen");
        return false;
    }

    make_non_blocking(listen_fd_);

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        std::perror("epoll_create1");
        return false;
    }

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = listen_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event) < 0) {
        std::perror("epoll_ctl");
        return false;
    }

    running_ = true;
    std::cout << "tinweb listening on http://127.0.0.1:" << port_
              << " serving " << root_ << std::endl;
    return true;
}

void Server::run() {
    epoll_event events[kMaxEvents];

    while (running_) {
        const int ready = ::epoll_wait(epoll_fd_, events, kMaxEvents, 500);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("epoll_wait");
            break;
        }

        for (int i = 0; i < ready; ++i) {
            const int fd = events[i].data.fd;
            const std::uint32_t flags = events[i].events;

            if (fd == listen_fd_) {
                accept_connections();
                continue;
            }

            if ((flags & (EPOLLERR | EPOLLHUP)) != 0) {
                close_connection(fd);
                continue;
            }

            if ((flags & EPOLLOUT) != 0) {
                handle_writable(fd);
            }
            if ((flags & (EPOLLIN | EPOLLRDHUP)) != 0 &&
                connections_.find(fd) != connections_.end()) {
                handle_readable(fd);
            }
        }
    }
}

void Server::stop() { running_ = false; }

void Server::accept_connections() {
    while (true) {
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EMFILE || errno == ENFILE) {
                std::cerr << "connection limit reached\n";
            }
            return;
        }

        if (static_cast<int>(connections_.size()) >= kMaxConnections) {
            ::close(fd);
            continue;
        }

        make_non_blocking(fd);
        connections_[fd] = Connection{};
        update_interest(fd, false);
    }
}

void Server::update_interest(int fd, bool want_write) {
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP;
    if (want_write) {
        event.events |= EPOLLOUT;
    }
    event.data.fd = fd;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) < 0) {
        // The descriptor is not registered yet: register it now.
        if (errno == ENOENT &&
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) == 0) {
            return;
        }
        close_connection(fd);
    }
}

void Server::handle_readable(int fd) {
    auto entry = connections_.find(fd);
    if (entry == connections_.end()) {
        return;
    }

    char buffer[16 * 1024];
    const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
    if (received == 0) {
        close_connection(fd);
        return;
    }
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return;
        }
        close_connection(fd);
        return;
    }

    Connection& connection = entry->second;
    connection.input.append(buffer, static_cast<std::size_t>(received));

    if (connection.input.size() > kMaxRequestSize) {
        connection.input.clear();
        queue_response(fd, build_response(431, "text/plain; charset=utf-8",
                                          "request too large\n", false, false),
                       true);
        return;
    }

    HttpRequest request;
    const ParseState state = parse_request(connection.input, request);
    if (state == ParseState::Incomplete) {
        return;
    }

    // Consume only the head: leftover bytes belong to the next request.
    const auto head_end = connection.input.find("\r\n\r\n");
    connection.input.erase(0, head_end + 4);

    if (state == ParseState::Error) {
        queue_response(fd, build_response(400, "text/plain; charset=utf-8",
                                          "bad request\n", false, false),
                       true);
        return;
    }

    const bool head_only = request.method == "HEAD";
    const bool get = request.method == "GET" || head_only;

    if (!get) {
        queue_response(fd, build_response(405, "text/plain; charset=utf-8",
                                          "method not allowed\n", request.keep_alive, false),
                       !request.keep_alive);
        std::cout << log_line(request.method, request.target, 405) << std::endl;
        return;
    }

    std::string path;
    if (!resolve_path(root_, request.target, path)) {
        queue_response(fd, build_response(400, "text/plain; charset=utf-8",
                                          "bad request\n", request.keep_alive, head_only),
                       !request.keep_alive);
        std::cout << log_line(request.method, request.target, 400) << std::endl;
        return;
    }

    std::string body;
    if (!read_file(path, body)) {
        queue_response(fd, build_response(404, "text/plain; charset=utf-8",
                                          "not found\n", request.keep_alive, head_only),
                       !request.keep_alive);
        std::cout << log_line(request.method, request.target, 404) << std::endl;
        return;
    }

    queue_response(fd,
                   build_response(200, mime_type(path), body, request.keep_alive, head_only),
                   !request.keep_alive);
    std::cout << log_line(request.method, request.target, 200) << std::endl;
}

void Server::queue_response(int fd, std::string response, bool close_after_write) {
    auto entry = connections_.find(fd);
    if (entry == connections_.end()) {
        return;
    }

    Connection& connection = entry->second;
    connection.output = std::move(response);
    connection.sent = 0;
    connection.close_after_write = close_after_write;

    handle_writable(fd);
}

void Server::handle_writable(int fd) {
    auto entry = connections_.find(fd);
    if (entry == connections_.end()) {
        return;
    }

    Connection& connection = entry->second;
    while (connection.sent < connection.output.size()) {
        const ssize_t written = ::send(fd, connection.output.data() + connection.sent,
                                       connection.output.size() - connection.sent,
                                       MSG_NOSIGNAL);
        if (written > 0) {
            connection.sent += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            update_interest(fd, true);
            return;
        }
        close_connection(fd);
        return;
    }

    if (connection.close_after_write) {
        close_connection(fd);
        return;
    }

    connection.output.clear();
    connection.sent = 0;
    update_interest(fd, false);
}

void Server::close_connection(int fd) {
    auto entry = connections_.find(fd);
    if (entry == connections_.end()) {
        return;
    }

    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    connections_.erase(entry);
}
