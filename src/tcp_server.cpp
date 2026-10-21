#include "netforge/frame_parser.hpp"
#include "netforge/socket_handle.hpp"
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <utility>
#include <vector>
#include <charconv>
#include <chrono>
#include <limits>
#include <string_view>
#include <system_error>
#include <optional>
#include <csignal>

namespace {

volatile std::sig_atomic_t shutdown_requested = 0;
volatile std::sig_atomic_t shutdown_pipe_write_fd = -1;

void handle_shutdown_signal(int)
{
    const int saved_errno = errno;

    shutdown_requested = 1;

    const int fd =
        static_cast<int>(shutdown_pipe_write_fd);

    if (fd >= 0) {
        const unsigned char signal_byte = 1;

        // write() is async-signal-safe.
        // The pipe is non-blocking, so this won't wait for space.
        (void)::write(fd, &signal_byte, sizeof(signal_byte));
    }

    errno = saved_errno;
}

struct SignalPipeWriteFdReset {
    ~SignalPipeWriteFdReset()
    {
        shutdown_pipe_write_fd = -1;
    }
};

constexpr std::uint16_t PORT = 54000;

constexpr std::size_t RECEIVE_CHUNK_SIZE = 4096;

constexpr std::size_t MAX_QUEUED_OUTPUT_BYTES =
    4U * 1024U * 1024U;

constexpr std::size_t MAX_CLIENTS = 100;

constexpr auto DEFAULT_IDLE_TIMEOUT =
    std::chrono::milliseconds{60'000};

constexpr auto DEFAULT_FRAME_TIMEOUT =
    std::chrono::milliseconds{5'000};

bool parse_timeout_value(
    std::string_view text,
    std::chrono::milliseconds& timeout)
{
    long long value = 0;

    const auto result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value);

    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() ||
        value < 1 ||
        value > std::numeric_limits<int>::max()) {
        return false;
    }

    timeout = std::chrono::milliseconds{value};
    return true;
}

bool parse_timeouts(
    int argc,
    char* argv[],
    std::chrono::milliseconds& idle_timeout,
    std::chrono::milliseconds& frame_timeout)
{
    idle_timeout = DEFAULT_IDLE_TIMEOUT;
    frame_timeout = DEFAULT_FRAME_TIMEOUT;

    int index = 1;

    while (index < argc) {
        const std::string_view option(argv[index++]);

        if (index >= argc) {
            return false;
        }

        const std::string_view value(argv[index++]);

        if (option == "--idle-timeout-ms") {
            if (!parse_timeout_value(value, idle_timeout)) {
                return false;
            }
        } else if (option == "--frame-timeout-ms") {
            if (!parse_timeout_value(value, frame_timeout)) {
                return false;
            }
        } else {
            return false;
        }
    }

    return true;
}

// Make a socket non-blocking.
bool set_nonblocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);

    if (flags == -1) {
        ::perror("fcntl(F_GETFL)");
        return false;
    }

    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        ::perror("fcntl(F_SETFL)");
        return false;
    }

    return true;
}

struct ClientConnection {
    explicit ClientConnection(netforge::SocketHandle&& handle)
        : socket(std::move(handle))
    {
    }

    netforge::SocketHandle socket;
    netforge::FrameParser parser;

    // Bytes waiting to be sent to this client.
    std::vector<std::uint8_t> pending_output;

    // Number of bytes already sent from pending_output.
    std::size_t output_offset{0};

    bool close_after_event{false};

    std::chrono::steady_clock::time_point last_activity{
        std::chrono::steady_clock::now()
    };

    bool timed_out{false};

    // When the current incomplete frame was first detected.
    std::optional<std::chrono::steady_clock::time_point>
        incomplete_frame_started;

    bool frame_timed_out{false};
};

bool accept_pending_connections(
    int server_fd,
    std::vector<ClientConnection>& clients)
{
    while (true) {
        const int accepted_fd =
            ::accept(server_fd, nullptr, nullptr);

        if (accepted_fd == -1) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No more connections waiting to be accepted.
                return true;
            }

            ::perror("accept");
            return false;
        }

        netforge::SocketHandle client_socket(accepted_fd);

        if (clients.size() >= MAX_CLIENTS) {
            std::cerr << "Maximum client count reached; rejecting connection\n";

            // client_socket automatically closes when this iteration ends.
            continue;
        }

        if (!set_nonblocking(client_socket.get()))
            continue;

        clients.emplace_back(std::move(client_socket));

        std::cout << "Client connected; active clients: " << clients.size() << '\n';
    }
}

} // namespace

int main(int argc, char* argv[])
{
    std::chrono::milliseconds idle_timeout;
    std::chrono::milliseconds frame_timeout;

    if (!parse_timeouts(
            argc,
            argv,
            idle_timeout,
            frame_timeout)) {
        std::cerr
            << "Usage: netforge_server "
            << "[--idle-timeout-ms MILLISECONDS] "
            << "[--frame-timeout-ms MILLISECONDS]\n";

        return 2;
    }

    std::cout << "Idle timeout: "
              << idle_timeout.count() << " ms\n";

    std::cout << "Incomplete-frame timeout: "
              << frame_timeout.count() << " ms\n";

    using netforge::SocketHandle;

    // Create a pipe that will wake poll() when shutdown is requested.
    int shutdown_pipe_fds[2];

    if (::pipe(shutdown_pipe_fds) == -1) {
        ::perror("pipe");
        return 1;
    }

    // Both descriptors are owned by RAII wrappers.
    SocketHandle shutdown_pipe_read(shutdown_pipe_fds[0]);
    SocketHandle shutdown_pipe_write(shutdown_pipe_fds[1]);

    // Both pipe ends must be non-blocking.
    if (!set_nonblocking(shutdown_pipe_read.get()) ||
        !set_nonblocking(shutdown_pipe_write.get())) {
        return 1;
    }

    shutdown_pipe_write_fd = shutdown_pipe_write.get();

    SignalPipeWriteFdReset reset_signal_pipe_write_fd;

    // Register handlers only after the pipe is ready.
    if (std::signal(SIGINT, handle_shutdown_signal) == SIG_ERR ||
        std::signal(SIGTERM, handle_shutdown_signal) == SIG_ERR) {
        std::cerr
            << "Failed to register shutdown signal handlers\n";
        return 1;
    }

    SocketHandle server_fd(
        ::socket(AF_INET, SOCK_STREAM, 0));

    if (!server_fd.valid()) {
        ::perror("socket");
        return 1;
    }

    int reuse_address = 1;

    if (::setsockopt(
            server_fd.get(),
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)) == -1) {
        ::perror("setsockopt");
        return 1;
    }

    if (!set_nonblocking(server_fd.get())) {
        return 1;
    }

    sockaddr_in server_address{};

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(PORT);
    server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(
            server_fd.get(),
            reinterpret_cast<sockaddr*>(&server_address),
            sizeof(server_address)) == -1) {
        ::perror("bind");
        return 1;
    }

    if (::listen(server_fd.get(), 10) == -1) {
        ::perror("listen");
        return 1;
    }

    std::cout << "NetForge poll server listening on "
              << "127.0.0.1:" << PORT << '\n';

    std::vector<ClientConnection> clients;

    while (!shutdown_requested) {
        std::vector<pollfd> poll_fds;
        std::vector<std::size_t> poll_to_client;

        poll_fds.push_back(pollfd{server_fd.get(), POLLIN, 0});

        // poll_fds[0]: listening socket
        // poll_fds[1]: shutdown pipe
        poll_fds.push_back(pollfd{shutdown_pipe_read.get(), POLLIN, 0});

        // Build a poll entry for every currently connected client.
        for (std::size_t i = 0; i < clients.size(); ++i) {
            short events = POLLIN;

            if (clients[i].output_offset <
                clients[i].pending_output.size()) {
                events = static_cast<short>(events | POLLOUT);
            }

            poll_fds.push_back(
                pollfd{clients[i].socket.get(), events, 0});

            // poll_fds[1] corresponds to poll_to_client[0].
            poll_to_client.push_back(i);
        }

        int poll_timeout_ms = -1;

        // If there are connected clients, wake up by the earliest
        // idle deadline rather than waiting indefinitely.
        if (!clients.empty()) {
            const auto now = std::chrono::steady_clock::now();

            auto nearest_deadline = clients.front().last_activity + idle_timeout;

            for (const auto& client : clients)
            {
                const auto idle_deadline = client.last_activity + idle_timeout;

                nearest_deadline = std::min(nearest_deadline, idle_deadline);

                if (client.incomplete_frame_started.has_value()) {
                    const auto frame_deadline = *client.incomplete_frame_started + frame_timeout;

                    nearest_deadline = std::min(nearest_deadline, frame_deadline);
                }
            }

            const auto remaining = nearest_deadline - now;

            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                poll_timeout_ms = 0;
            } else {
                auto remaining_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        remaining).count();

                // Avoid rounding a positive wait down to zero.
                if (remaining_ms == 0) {
                    remaining_ms = 1;
                }

                poll_timeout_ms = static_cast<int>(remaining_ms);
            }
        }

        const int ready = ::poll(
            poll_fds.data(),
            static_cast<nfds_t>(poll_fds.size()),
            poll_timeout_ms);

        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            }

            ::perror("poll");
            return 1;
        }

        // ---------- HANDLE SHUTDOWN PIPE ----------
        if (poll_fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::cerr << "Shutdown pipe error\n";
            return 1;
        }

        if (poll_fds[1].revents & POLLIN) {
            unsigned char buffer[64];

            // Drain all available bytes. The pipe is non-blocking.
            while (true) {
                const ssize_t bytes_read = ::read(
                    shutdown_pipe_read.get(),
                    buffer,
                    sizeof(buffer));

                if (bytes_read > 0) {
                    continue;
                }

                if (bytes_read == -1 && errno == EINTR) {
                    continue;
                }

                if (bytes_read == -1 &&
                    (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }

                if (bytes_read == 0) {
                    break;
                }

                ::perror("read shutdown pipe");
                return 1;
            }
        }

        // Exit before processing any more client or listener events.
        if (shutdown_requested) {
            break;
        }

        const auto timeout_check_time = std::chrono::steady_clock::now();

        for (auto& client : clients) {
            if (client.close_after_event)
                continue;

            const bool idle_expired = timeout_check_time - client.last_activity >= idle_timeout;

            const bool frame_expired = client.incomplete_frame_started.has_value() &&
                timeout_check_time - *client.incomplete_frame_started >= frame_timeout;

            if (frame_expired) {
                client.close_after_event = true;
                client.frame_timed_out = true;
            } else if (idle_expired) {
                client.close_after_event = true;
                client.timed_out = true;
            }
        }

        // The listening socket is always poll_fds[0].
        if (poll_fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::cerr << "Listening socket error\n";
            return 1;
        }

        if (poll_fds[0].revents & POLLIN) {
            if (!accept_pending_connections(
                    server_fd.get(), clients)) {
                return 1;
            }
        }

        // Process events for the clients that were present when
        // this poll() call was prepared.
        for (std::size_t p = 2; p < poll_fds.size(); ++p) {
            if (poll_fds[p].revents == 0) {
                continue;
            }

            const std::size_t client_index = poll_to_client[p - 2];

            ClientConnection& client = clients[client_index];

            if (client.close_after_event)
                continue;

            // ---------- READ AVAILABLE DATA ----------
            if (poll_fds[p].revents & POLLIN) {
                while (!client.close_after_event) {
                    std::uint8_t buffer[RECEIVE_CHUNK_SIZE];

                    const ssize_t bytes_read = ::recv(
                        client.socket.get(),
                        buffer,
                        sizeof(buffer),
                        0);

                    if (bytes_read > 0) {

                        client.last_activity = std::chrono::steady_clock::now();

                        std::string error;

                        if (!client.parser.append(
                                buffer,
                                static_cast<std::size_t>(bytes_read),
                                error)) {
                            std::cerr << "Input buffering failed: "
                                      << error << '\n';

                            client.close_after_event = true;
                            break;
                        }

                        // Process every complete frame currently
                        // available, not just the first one.
                        while (!client.close_after_event) {
                            auto decoded = client.parser.next_frame();

                        if (decoded.status == netforge::DecodeStatus::NeedMoreData)
                        {
                            if (client.parser.buffered_bytes() == 0)
                            {
                                client.incomplete_frame_started.reset();
                            } else if (!client.incomplete_frame_started.has_value())
                            {
                                client.incomplete_frame_started = std::chrono::steady_clock::now();
                            }
                            break;
                        }

                            if (decoded.status ==
                                netforge::DecodeStatus::Invalid) {
                                std::cerr << "Invalid frame: "
                                          << decoded.error << '\n';

                                client.close_after_event = true;
                                break;
                            }

                            // A complete frame was decoded.
                            // Any deadline for that frame is no longer needed.
                            client.incomplete_frame_started.reset();

                            netforge::Frame request = std::move(decoded.frame);

                            if (request.type !=
                                netforge::MessageType::Chat) {
                                std::cerr
                                    << "Only CHAT frames are supported\n";

                                client.close_after_event = true;
                                break;
                            }

                            std::cout << "Received request "
                                      << request.request_id
                                      << ", payload size: "
                                      << request.payload.size()
                                      << " bytes\n";

                            netforge::Frame response;
                            response.type =
                                netforge::MessageType::Chat;
                            response.request_id = request.request_id;
                            response.payload = request.payload;

                            const auto encoded =
                                netforge::encode_frame(response);

                            // Discard the prefix that has already
                            // been sent before appending more bytes.
                            if (client.output_offset > 0) {
                                client.pending_output.erase(
                                    client.pending_output.begin(),
                                    client.pending_output.begin() +
                                        static_cast<std::ptrdiff_t>(
                                            client.output_offset));

                                client.output_offset = 0;
                            }

                            // Prevent a slow client from accumulating
                            // an unlimited response queue.
                            if (encoded.size() >
                                MAX_QUEUED_OUTPUT_BYTES -
                                    client.pending_output.size()) {
                                std::cerr
                                    << "Output queue limit exceeded; "
                                    << "closing slow client\n";

                                client.close_after_event = true;
                                break;
                            }

                            client.pending_output.insert(
                                client.pending_output.end(),
                                encoded.begin(),
                                encoded.end());
                        }

                        if (client.close_after_event) {
                            break;
                        }

                        // Try to read more until recv() would block.
                        continue;
                    }

                    if (bytes_read == 0) {
                        client.close_after_event = true;
                        break;
                    }

                    if (errno == EINTR) {
                        continue;
                    }

                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // All currently available data has been read.
                        break;
                    }

                    ::perror("recv");
                    client.close_after_event = true;
                    break;
                }
            }

            // ---------- WRITE QUEUED DATA ----------
            if ((poll_fds[p].revents & POLLOUT) &&
                !client.close_after_event) {

                while (client.output_offset <
                       client.pending_output.size()) {

                    const ssize_t bytes_sent = ::send(
                        client.socket.get(),
                        client.pending_output.data() +
                            client.output_offset,
                        client.pending_output.size() -
                            client.output_offset,
                        MSG_NOSIGNAL);

                    if (bytes_sent > 0) {
                        client.output_offset +=
                            static_cast<std::size_t>(bytes_sent);
                        continue;
                    }

                    if (bytes_sent == -1 && errno == EINTR) {
                        continue;
                    }

                    if (bytes_sent == -1 &&
                        (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        // Keep the unsent bytes for a future POLLOUT.
                        break;
                    }

                    if (bytes_sent == 0) {
                        std::cerr << "send returned zero\n";
                    } else {
                        ::perror("send");
                    }

                    client.close_after_event = true;
                    break;
                }

                if (client.output_offset ==
                    client.pending_output.size()) {
                    client.pending_output.clear();
                    client.output_offset = 0;
                }
            }

            if (poll_fds[p].revents &
                (POLLERR | POLLHUP | POLLNVAL)) {
                client.close_after_event = true;
            }
        }

        // Remove clients only after processing all events. Removing
        // one in the middle of the loop would invalidate indexes.
        for (const auto& client : clients) {
            if (!client.close_after_event)
                continue;

            if (client.frame_timed_out)
                std::cout<< "Client timed out while sending an incomplete frame\n";
            else if (client.timed_out)
                std::cout << "Client timed out after inactivity\n";
            else
                std::cout << "Client disconnected\n";
        }

        clients.erase(
            std::remove_if(
                clients.begin(),
                clients.end(),
                [](const ClientConnection& client) {
                    return client.close_after_event;
                }),
            clients.end());
    }

    std::cout << "Shutdown requested; closing server and client sockets\n";

    return 0;
}