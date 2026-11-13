# TinWebServer

A small, dependency-free HTTP/1.1 static file server written from scratch in
**C++20** on top of `epoll`. No frameworks, no third-party libraries — just one
binary and a directory of files to serve.

```
./tinweb --port 8080 --root www
```

---

## Table of contents

- [Features](#features)
- [Requirements](#requirements)
- [Build](#build)
- [Run](#run)
- [Testing](#testing)
- [Project layout](#project-layout)
- [Architecture](#architecture)
- [Design notes](#design-notes)
- [Limitations](#limitations)
- [License](#license)

---

## Features

- Single-threaded, level-triggered `epoll` event loop.
- Fully non-blocking sockets with correct handling of partial reads and writes
  (backpressure via `EPOLLOUT`).
- `GET` and `HEAD` methods.
- Persistent connections (HTTP/1.1 keep-alive) with `Connection: close`
  support, plus correct HTTP/1.0 defaults.
- MIME type detection for the common web formats.
- Path traversal protection: any target that resolves outside the document root
  is rejected with `400 Bad Request`.
- Request size limit (64 KiB) and a hard cap on concurrent connections (1024).
- Access log to `stdout`.
- Graceful shutdown on `SIGINT` / `SIGTERM`.
- No external dependencies; builds with a single `make`.

## Requirements

- A C++20-capable compiler (`g++` or `clang++`).
- `make`.
- A POSIX system with `epoll` (Linux).

## Build

```bash
make
```

This produces the `tinweb` binary in the repository root. Object files and
dependency files are written to `build/`.

Useful targets:

| Target | Description |
| ------ | ----------- |
| `make` | Build the `tinweb` binary |
| `make run` | Build and run on port 8080 serving `www` |
| `make test` | Build and run the unit tests |
| `make clean` | Remove `build/` and the binary |

The build is configurable through standard variables:

```bash
make CXX=clang++ CXXFLAGS="-std=c++20 -O2 -Wall -Wextra -Iinclude"
```

## Run

```bash
./tinweb --port 8080 --root www
```

| Flag | Short | Meaning | Default |
| ---- | ----- | ------- | ------- |
| `--root` | `-r` | Directory to serve | `www` |
| `--port` | `-p` | TCP port to listen on | `8080` |
| `--help` | `-h` | Print usage and exit | — |

Then open <http://127.0.0.1:8080>.

## Testing

The project ships with a self-contained unit-test binary (no test framework
required) and an end-to-end smoke test that runs in CI.

### Unit tests

```bash
make test
```

The tests in `tests/http_test.cpp` cover the request parser and path handling:

- Incomplete request detection (head not yet terminated).
- Full request parsing (method, target, path, version, headers, keep-alive).
- Query-string splitting from the path.
- `Connection: close` handling and HTTP/1.0 keep-alive defaults.
- Malformed request rejection.
- `url_decode` (`%XX` escapes and `+`).
- MIME type mapping (including unknown extensions).
- `resolve_path` for files, directories and query strings.
- Path traversal rejection (`..`, encoded `%2e%2e`, missing leading slash).

Expected output on success:

```
N checks, 0 failure(s)
```

### Manual smoke test

With the server running:

```bash
curl -i  http://127.0.0.1:8080/                       # 200 index.html
curl -I  http://127.0.0.1:8080/style.css              # 200, headers only
curl -i  http://127.0.0.1:8080/missing                # 404
curl -i --path-as-is http://127.0.0.1:8080/../etc/passwd   # 400
```

> `curl` normalises `/../` unless `--path-as-is` is given, so the traversal
> check must send the raw target.

### Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request:

1. `make` — build the server.
2. `make test` — run the unit tests.
3. A smoke test that boots the server, exercises the endpoints above, asserts
   the status codes, and dumps the server log.

## Project layout

```
include/http.h    Request parser, MIME types, path resolution
include/server.h  Event loop and connection bookkeeping
src/http.cpp      Parser and path handling
src/server.cpp    epoll loop, socket lifecycle, responses
src/main.cpp      CLI parsing and signal handling
tests/http_test.cpp  Unit tests for the HTTP module
www/              Example document root (index.html, style.css)
Makefile          Build, test and run targets
```

## Architecture

The server is intentionally split into a pure, testable HTTP layer and an
I/O-driven server core.

### `http` — parsing and path handling (`include/http.h`, `src/http.cpp`)

- `parse_request(buffer, request)` parses the request head (everything up to and
  including the blank line) into an `HttpRequest`. It returns `Incomplete` while
  the head has not fully arrived, `Done` on success, and `Error` on a malformed
  request. This lets clients send a request across several packets without extra
  state.
- `url_decode(text)` decodes `%XX` escapes and `+`.
- `mime_type(path)` maps a file extension to a MIME type.
- `resolve_path(root, target, out)` turns a request target into a path inside the
  document root and rejects anything that would escape it.

### `Server` — event loop (`include/server.h`, `src/server.cpp`)

- `listen_and_serve()` creates the listening socket, binds, listens, and
  registers the listener with `epoll`.
- `run()` drives the level-triggered event loop until `stop()` is called. It
  accepts new connections, reads requests, and writes responses, keeping per-FD
  state in a `std::unordered_map<int, Connection>`.
- `handle_readable()` accumulates bytes, enforces the 64 KiB request limit
  (`431`), parses the head, dispatches the request, and keeps any leftover bytes
  for the next request on the same connection.
- `handle_writable()` drains the response buffer and, if the socket would block,
  switches the FD's interest to include `EPOLLOUT` — the core of the
  backpressure handling.

### Request flow

```
accept() -> recv() -> append to per-FD buffer
                     |
                     +- head incomplete -------------> wait for more bytes
                     +- > 64 KiB --------------------> 431 Request Header Fields Too Large
                     +- parse error -----------------> 400 Bad Request
                     +- method not GET/HEAD ---------> 405 Method Not Allowed
                     +- path escapes root -----------> 400 Bad Request
                     +- file not found --------------> 404 Not Found
                     +- else ------------------------> 200 OK (body for GET, headers for HEAD)
                                                      |
                              queue_response() -------+-> send() until drained
```

Status codes emitted: `200`, `400`, `403`, `404`, `405`, `431`, `500`.

## Design notes

- **Single-threaded on purpose.** Keeping all connection state in one place
  makes backpressure easy to reason about. Serving static files from disk on the
  same thread is fine at this scale.
- **Parse only complete heads.** A request is parsed only once the full head
  (`\r\n\r\n`) has arrived, so fragmented requests work without extra state.
- **Leftover bytes are preserved.** Only the head is consumed; bytes belonging
  to a pipelined follow-up request stay in the buffer.
- **Responses are written straight from memory.** For very large files a
  `sendfile` path would be the natural next step.

## Limitations

- No TLS, no CGI, no range requests, no compression.
- One document root per process.
- Linux/`epoll` only.

## License

No license file is currently included in this repository.
