# tinweb

A small HTTP/1.1 static file server written from scratch in C++20 on top of
`epoll`. No frameworks, no external dependencies: one binary and a directory of
files to serve.

## Features

- Single-threaded event loop with level-triggered `epoll`
- Non-blocking sockets, partial reads and writes handled correctly
- `GET` and `HEAD`
- Persistent connections (HTTP/1.1 keep-alive), with `Connection: close` support
- MIME type detection for the common web formats
- Path traversal protection: any target that resolves outside the document root
  is rejected
- Request size limit (64 KiB) and a hard cap on concurrent connections
- Access log to stdout

## Build

```bash
make
```

Requires `g++` with C++20 support and `make`.

## Run

```bash
./tinweb --port 8080 --root www
```

Options:

| Flag | Meaning |
| ---- | ------- |
| `-r`, `--root` | directory to serve (default `www`) |
| `-p`, `--port` | TCP port (default `8080`) |
| `-h`, `--help` | usage |

Then open http://127.0.0.1:8080.

## Test it

```bash
curl -i http://127.0.0.1:8080/            # 200 index.html
curl -I http://127.0.0.1:8080/style.css   # 200, headers only
curl -i http://127.0.0.1:8080/missing     # 404
curl -i --path-as-is http://127.0.0.1:8080/../etc/passwd   # 400
```

## Layout

```
include/http.h    request parser, MIME types, path resolution
include/server.h  event loop and connection bookkeeping
src/http.cpp      parser and path handling
src/server.cpp    epoll loop, socket lifecycle, responses
src/main.cpp      CLI and signal handling
www/              example document root
```

## Design notes

- The event loop is single-threaded on purpose: it keeps the connection state
  in one place and makes backpressure easy to reason about. Serving from disk on
  the same thread is fine for static files at this scale.
- Requests are parsed only once the full head (`\r\n\r\n`) has arrived, so
  clients that send a request in several packets work without extra state.
- Responses are written straight from memory; for very large files a `sendfile`
  path would be the next step.

## Limitations

- No TLS, no CGI, no range requests, no compression.
- One document root per process.
