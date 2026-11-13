#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#include "server.h"

namespace {

volatile std::sig_atomic_t g_running = 1;

void handle_signal(int) { g_running = 0; }

void print_usage() {
    std::cout << "tinweb: small HTTP/1.1 static file server\n\n"
                 "Usage:\n"
                 "  tinweb [--root DIR] [--port PORT]\n\n"
                 "Options:\n"
                 "  -r, --root DIR   directory to serve (default: www)\n"
                 "  -p, --port PORT  TCP port to listen on (default: 8080)\n"
                 "  -h, --help       show this help\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string root = "www";
    int port = 8080;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];

        if ((argument == "--root" || argument == "-r") && i + 1 < argc) {
            root = argv[++i];
        } else if ((argument == "--port" || argument == "-p") && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (argument == "--help" || argument == "-h") {
            print_usage();
            return 0;
        } else {
            std::cerr << "unknown argument: " << argument << "\n\n";
            print_usage();
            return 2;
        }
    }

    if (port <= 0 || port > 65535) {
        std::cerr << "invalid port: " << port << '\n';
        return 2;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGPIPE, SIG_IGN);

    Server server(root, port);
    if (!server.listen_and_serve()) {
        return 1;
    }

    server.run();
    server.stop();
    std::cout << "shutting down\n";
    return 0;
}
