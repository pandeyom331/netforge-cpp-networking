# NetForge — C++ TCP Networking Framework

NetForge is a C++17 networking project built to explore reliable TCP communication, binary protocol design, non-blocking I/O, and robust connection management on Linux.

The project includes a custom binary framing protocol, an incremental frame parser, a non-blocking TCP server built around `poll()`, and automated tests.

## Features

- **Custom binary protocol:** Explicit message types, request IDs, protocol versioning, and big-endian header fields.
- **Incremental frame parsing:** Handles fragmented frames and multiple frames received together.
- **Non-blocking TCP server:** Uses POSIX sockets and `poll()` to manage multiple clients in a single event loop.
- **Resource management:** Move-only RAII socket wrapper for automatic file-descriptor cleanup.
- **Connection safeguards:** Maximum client count, bounded output queues, idle timeouts, and incomplete-frame timeouts.
- **Graceful shutdown:** Handles `SIGINT` and `SIGTERM` using a self-pipe to wake the event loop.
- **Automated testing:** Unit tests for protocol encoding and frame parsing, plus server integration tests.
- **Continuous integration:** GitHub Actions builds the project and runs its test suite.

## Architecture

```text
TCP Client
    |
    | TCP byte stream
    v
Non-blocking TCP Server
    |
    +-- poll() event loop
    |
    +-- Client connection management
    |
    +-- Incremental Frame Parser
    |       |
    |       v
    |   Binary Protocol Decoder
    |
    +-- Response queue
            |
            v
       TCP Client
```

## Protocol Overview

Each frame consists of a fixed 12-byte header followed by a variable-length payload.

| Field | Size |
|---|---:|
| Magic identifier | 2 bytes |
| Protocol version | 1 byte |
| Message type | 1 byte |
| Request ID | 4 bytes |
| Payload length | 4 bytes |
| Payload | Variable |

Header integer fields are encoded in big-endian byte order. The maximum payload size is 1 MiB.

Supported message types include `HELLO`, `CHAT`, `LIST_USERS`, `FILE_BEGIN`, `FILE_CHUNK`, `FILE_END`, `ERROR`, `PING`, and `PONG`. The current TCP server implements the `CHAT` echo path; the other message types are protocol definitions, not yet fully implemented server features.

## Requirements

- Linux or a compatible POSIX environment
- C++17 compiler
- CMake
- POSIX sockets and `poll()`

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
```

## Run the Server

```bash
./build/netforge_server
```

The server listens on `127.0.0.1:54000` by default.

Optional timeout settings:

```bash
./build/netforge_server \
  --idle-timeout-ms 60000 \
  --frame-timeout-ms 5000
```

## Run the Client

Start the server in one terminal, then run the client in another:

```bash
./build/netforge_client
```

Enter a message to send it to the server. Enter `/quit` to exit.

## Run Tests

```bash
ctest --test-dir build --output-on-failure
```

The test suite covers protocol encoding and decoding, incomplete and invalid frames, frame fragmentation, multiple frames in one receive buffer, client isolation, client limits, timeout behavior, and graceful server shutdown.

## Project Structure

```text
.
├── .github/workflows/ci.yml
├── examples/echo/
├── include/netforge/
├── src/
├── tests/
├── CMakeLists.txt
└── README.md
```

## Technical Focus

This project explores modern C++ resource ownership, TCP stream semantics, binary protocol design, non-blocking I/O, event-driven networking, timeout management, POSIX signal handling, and integration testing.

## Future Improvements

- Implement additional message types.
- Add a networking load test and performance benchmarks.
- Add sanitizers to the CI pipeline.
- Expand the protocol with authentication and stronger input validation.
