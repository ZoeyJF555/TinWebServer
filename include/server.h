#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <unordered_map>

class Server {
public:
    Server(std::string root, int port);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Creates the socket, binds and starts listening.
    bool listen_and_serve();

    // Runs the event loop until stop() is called.
    void run();

    void stop();

private:
    struct Connection {
        std::string input;
        std::string output;
        std::size_t sent = 0;
        bool close_after_write = false;
    };

    void accept_connections();
    void handle_readable(int fd);
    void handle_writable(int fd);
    void close_connection(int fd);
    void queue_response(int fd, std::string response, bool close_after_write);
    void update_interest(int fd, bool want_write);

    int listen_fd_ = -1;
    int epoll_fd_ = -1;
    int port_;
    std::string root_;
    std::unordered_map<int, Connection> connections_;
    std::atomic<bool> running_{false};
};
