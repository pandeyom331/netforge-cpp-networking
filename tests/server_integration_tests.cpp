#include "netforge/frame_parser.hpp"
#include "netforge/protocol.hpp"
#include "netforge/socket_handle.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>

namespace {

constexpr std::uint16_t PORT = 54000;
constexpr int RECEIVE_TIMEOUT_SECONDS = 3;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Start the actual server as a separate operating-system process.
class ServerProcess {
public:
    explicit ServerProcess(
        const char* executable,
        const char* idle_timeout_ms = nullptr,
        const char* frame_timeout_ms = nullptr)
    {
        pid_ = ::fork();

        if (pid_ == -1) {
            throw std::runtime_error("fork() failed");
        }

        if (pid_ == 0) {
            // Child process: launch the server with the requested options.
            if (idle_timeout_ms != nullptr &&
                frame_timeout_ms != nullptr) {

                ::execl(
                    executable,
                    executable,
                    "--idle-timeout-ms",
                    idle_timeout_ms,
                    "--frame-timeout-ms",
                    frame_timeout_ms,
                    static_cast<char*>(nullptr));

            } else if (idle_timeout_ms != nullptr) {

                ::execl(
                    executable,
                    executable,
                    "--idle-timeout-ms",
                    idle_timeout_ms,
                    static_cast<char*>(nullptr));

            } else if (frame_timeout_ms != nullptr) {

                ::execl(
                    executable,
                    executable,
                    "--frame-timeout-ms",
                    frame_timeout_ms,
                    static_cast<char*>(nullptr));

            } else {

                ::execl(
                    executable,
                    executable,
                    static_cast<char*>(nullptr));
            }

            ::perror("execl");
            ::_exit(127);
        }

        // Parent process: allow the server to start.
        std::this_thread::sleep_for(
            std::chrono::milliseconds(150));

        int status = 0;
        const pid_t result = ::waitpid(pid_, &status, WNOHANG);

        if (result == pid_) {
            pid_ = -1;

            throw std::runtime_error(
                "Server exited during startup. Check port 54000 "
                "and the server executable.");
        }

        if (result == -1) {
            pid_ = -1;
            throw std::runtime_error(
                "waitpid() failed during startup");
        }
    }

    ServerProcess(const ServerProcess&) = delete;
    ServerProcess& operator=(const ServerProcess&) = delete;

    ~ServerProcess()
    {
        stop();
    }

    void stop() noexcept
    {
        if (pid_ <= 0) {
            return;
        }

        ::kill(pid_, SIGTERM);

        int status = 0;

        while (::waitpid(pid_, &status, 0) == -1) {
            if (errno != EINTR) {
                break;
            }
        }

        pid_ = -1;
    }

private:
    pid_t pid_{-1};
};

// Give each client a receive timeout so a broken server cannot
// make an integration test hang indefinitely.
void set_receive_timeout(int fd)
{
    timeval timeout{};
    timeout.tv_sec = RECEIVE_TIMEOUT_SECONDS;

    if (::setsockopt(
            fd,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            sizeof(timeout)) == -1) {
        throw std::runtime_error(
            "Could not configure client receive timeout");
    }
}

netforge::SocketHandle connect_to_server()
{
    for (int attempt = 0; attempt < 60; ++attempt) {
        netforge::SocketHandle socket_fd(
            ::socket(AF_INET, SOCK_STREAM, 0));

        if (!socket_fd.valid()) {
            throw std::runtime_error("socket() failed");
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(PORT);

        if (::inet_pton(
                AF_INET,
                "127.0.0.1",
                &address.sin_addr) != 1) {
            throw std::runtime_error("inet_pton() failed");
        }

        if (::connect(
                socket_fd.get(),
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == 0) {
            set_receive_timeout(socket_fd.get());
            return socket_fd;
        }

        if (errno != ECONNREFUSED &&
            errno != EINTR &&
            errno != EAGAIN) {
            throw std::runtime_error(
                std::string("connect() failed: ") +
                std::strerror(errno));
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(50));
    }

    throw std::runtime_error(
        "Could not connect to the test server on port 54000");
}

bool send_all(
    int fd,
    const std::uint8_t* data,
    std::size_t size)
{
    std::size_t sent = 0;

    while (sent < size) {
        const ssize_t result = ::send(
            fd,
            data + sent,
            size - sent,
            MSG_NOSIGNAL);

        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }

        if (result == -1 && errno == EINTR) {
            continue;
        }

        return false;
    }

    return true;
}

void send_bytes(
    int fd,
    const std::vector<std::uint8_t>& bytes)
{
    require(
        send_all(fd, bytes.data(), bytes.size()),
        "Failed to send test data");
}

netforge::Frame make_chat_frame(
    std::uint32_t request_id,
    const std::string& message)
{
    netforge::Frame frame;
    frame.type = netforge::MessageType::Chat;
    frame.request_id = request_id;
    frame.payload.assign(message.begin(), message.end());

    return frame;
}

void send_frame(int fd, const netforge::Frame& frame)
{
    const auto bytes = netforge::encode_frame(frame);
    send_bytes(fd, bytes);
}

netforge::Frame receive_frame(
    int fd,
    netforge::FrameParser& parser)
{
    std::uint8_t buffer[4096];

    while (true) {
        auto result = parser.next_frame();

        if (result.status == netforge::DecodeStatus::Complete) {
            return std::move(result.frame);
        }

        if (result.status == netforge::DecodeStatus::Invalid) {
            throw std::runtime_error(
                "Server response contained an invalid frame: " +
                result.error);
        }

        const ssize_t bytes_read = ::recv(
            fd,
            buffer,
            sizeof(buffer),
            0);

        if (bytes_read == 0) {
            throw std::runtime_error(
                "Server disconnected before sending a complete response");
        }

        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }

            throw std::runtime_error(
                std::string("recv() failed: ") +
                std::strerror(errno));
        }

        std::string error;

        if (!parser.append(
                buffer,
                static_cast<std::size_t>(bytes_read),
                error)) {
            throw std::runtime_error(
                "Could not buffer server response: " + error);
        }
    }
}

void expect_echo(
    int fd,
    netforge::FrameParser& parser,
    const netforge::Frame& request)
{
    const auto response = receive_frame(fd, parser);

    require(
        response.type == netforge::MessageType::Chat,
        "Response has the wrong message type");

    require(
        response.request_id == request.request_id,
        "Response request ID does not match the request");

    require(
        response.payload == request.payload,
        "Response payload does not match the request");
}

// Scenario 1: an idle connection must not stop another client.
void test_idle_client_does_not_block()
{
    auto idle_client = connect_to_server();
    auto active_client = connect_to_server();

    netforge::FrameParser idle_parser;
    netforge::FrameParser active_parser;

    const auto request = make_chat_frame(1, "active client works");

    send_frame(active_client.get(), request);

    // This would time out if the server were blocked waiting for
    // the first, idle client.
    expect_echo(active_client.get(), active_parser, request);

    const auto idle_request = make_chat_frame(1, "I was idle");

    send_frame(idle_client.get(), idle_request);
    expect_echo(idle_client.get(), idle_parser, idle_request);

    std::cout << "[PASS] Idle client does not block active client\n";
}

// Scenario 2: two frames sent together must both be handled.
void test_two_frames_in_one_send()
{
    auto client = connect_to_server();
    netforge::FrameParser parser;

    const auto first = make_chat_frame(10, "first frame");
    const auto second = make_chat_frame(11, "second frame");

    const auto first_bytes = netforge::encode_frame(first);
    const auto second_bytes = netforge::encode_frame(second);

    std::vector<std::uint8_t> combined;
    combined.insert(
        combined.end(), first_bytes.begin(), first_bytes.end());
    combined.insert(
        combined.end(), second_bytes.begin(), second_bytes.end());

    send_bytes(client.get(), combined);

    expect_echo(client.get(), parser, first);
    expect_echo(client.get(), parser, second);

    std::cout << "[PASS] Two coalesced frames receive two responses\n";
}

// Scenario 3: a partial frame must not trigger a premature response.
void test_fragmented_frame()
{
    auto client = connect_to_server();
    netforge::FrameParser parser;

    const auto request = make_chat_frame(
        77, "fragmented network frame");

    const auto bytes = netforge::encode_frame(request);

    // Send only five bytes, splitting the protocol header.
    constexpr std::size_t FIRST_FRAGMENT_SIZE = 5;

    require(
        bytes.size() > FIRST_FRAGMENT_SIZE,
        "Test frame is unexpectedly small");

    require(
        send_all(
            client.get(),
            bytes.data(),
            FIRST_FRAGMENT_SIZE),
        "Failed to send first fragment");

    // Give the server an opportunity to process the incomplete input.
    pollfd descriptor{client.get(), POLLIN, 0};

    int poll_result;

    do {
        poll_result = ::poll(&descriptor, 1, 150);
    } while (poll_result == -1 && errno == EINTR);

    require(poll_result != -1, "poll() failed in fragmented-frame test");

    require(
        poll_result == 0,
        "Server responded before receiving the full frame");

    // Send the remainder; the server should now echo the full frame.
    const auto remaining_size = bytes.size() - FIRST_FRAGMENT_SIZE;

    require(
        send_all(
            client.get(),
            bytes.data() + FIRST_FRAGMENT_SIZE,
            remaining_size),
        "Failed to send remaining fragment");

    expect_echo(client.get(), parser, request);

    std::cout << "[PASS] Fragmented frame is processed correctly\n";
}

// Scenario 4: malformed input closes that connection, not the server.
void test_invalid_frame_does_not_stop_server()
{
    {
        auto client = connect_to_server();

        const std::uint8_t invalid_magic[] = {'X', 'Y'};

        require(
            send_all(
                client.get(),
                invalid_magic,
                sizeof(invalid_magic)),
            "Failed to send invalid frame");

        char buffer[1]{};

        while (true) {
            const ssize_t result = ::recv(
                client.get(),
                buffer,
                sizeof(buffer),
                0);

            if (result == 0) {
                // EOF: server closed the connection.
                break;
            }

            if (result < 0 && errno == EINTR) {
                continue;
            }

            if (result < 0 && errno == ECONNRESET) {
                // A reset is also an acceptable connection close.
                break;
            }

            if (result < 0) {
                throw std::runtime_error(
                    std::string("Invalid-frame connection did not close: ") +
                    std::strerror(errno));
            }

            throw std::runtime_error(
                "Server unexpectedly sent a response to invalid input");
        }
    }

    // A completely new connection should still work.
    auto healthy_client = connect_to_server();
    netforge::FrameParser parser;

    const auto request = make_chat_frame(99, "server still healthy");

    send_frame(healthy_client.get(), request);
    expect_echo(healthy_client.get(), parser, request);

    std::cout << "[PASS] Invalid frame closes only its client connection\n";
}

void test_maximum_client_limit()
{
    std::vector<netforge::SocketHandle> clients;
    clients.reserve(100);

    // Fill all 100 permitted client slots.
    for (std::size_t i = 0; i < 100; ++i) {
        clients.push_back(connect_to_server());
    }

    // This connection should be accepted by the TCP stack,
    // then closed by the server's application-level client limit.
    auto excess_client = connect_to_server();

    char buffer[1]{};

    while (true) {
        const ssize_t result = ::recv(
            excess_client.get(),
            buffer,
            sizeof(buffer),
            0);

        if (result == 0) {
            break; // Expected EOF: server closed the connection.
        }

        if (result < 0 && errno == EINTR) {
            continue;
        }

        if (result < 0 && errno == ECONNRESET) {
            break; // A connection reset is also an acceptable close.
        }

        require(
            false,
            "Server did not close the excess client connection");
    }

    std::cout << "[PASS] Maximum client limit is enforced\n";
}

void test_idle_client_times_out()
{
    // This server must be launched with a short idle timeout.
    auto client = connect_to_server();

    pollfd descriptor{client.get(), POLLIN, 0};

    int result;

    do {
        result = ::poll(&descriptor, 1, 2000);
    } while (result == -1 && errno == EINTR);

    require(result != -1, "poll() failed in idle-timeout test");

    require(
        result > 0,
        "Server did not close the idle connection within 2 seconds");

    char buffer[1]{};

    const ssize_t bytes_read = ::recv(
        client.get(),
        buffer,
        sizeof(buffer),
        0);

    require(
        bytes_read == 0 ||
            (bytes_read == -1 && errno == ECONNRESET),
        "Expected the server to close the idle client connection");

    std::cout << "[PASS] Idle client is disconnected after timeout\n";
}

void test_incomplete_frame_times_out_despite_trickled_bytes()
{
    auto client = connect_to_server();

    // Encode a valid frame, but deliberately send only its first
    // three bytes. The server must wait for the rest of the header.
    const auto frame = make_chat_frame(99, "partial message");
    const auto encoded = netforge::encode_frame(frame);

    require(
        encoded.size() >= 3,
        "Encoded frame is unexpectedly short");

    require(
        send_all(client.get(), encoded.data(), 1),
        "Failed to send the first partial-frame byte");

    // Send additional bytes before the 1500 ms frame timeout expires.
    std::this_thread::sleep_for(
        std::chrono::milliseconds(350));

    require(
        send_all(client.get(), encoded.data() + 1, 1),
        "Failed to send the second partial-frame byte");

    std::this_thread::sleep_for(
        std::chrono::milliseconds(350));

    require(
        send_all(client.get(), encoded.data() + 2, 1),
        "Failed to send the third partial-frame byte");

    // The frame deadline is measured from the first byte, not
    // restarted every time another byte arrives.
    pollfd descriptor{client.get(), POLLIN, 0};

    int result;

    do {
        result = ::poll(&descriptor, 1, 1000);
    } while (result == -1 && errno == EINTR);

    require(
        result != -1,
        "poll() failed in incomplete-frame timeout test");

    require(
        result > 0,
        "Server did not close the incomplete frame connection "
        "within the expected deadline");

    char buffer[1]{};

    const ssize_t bytes_read = ::recv(
        client.get(),
        buffer,
        sizeof(buffer),
        0);

    require(
        bytes_read == 0 ||
            (bytes_read == -1 && errno == ECONNRESET),
        "Expected the server to close the incomplete-frame connection");

    // Verify that the timeout affected only the offending client.
    auto active_client = connect_to_server();
    netforge::FrameParser parser;

    const auto request =
        make_chat_frame(100, "server remains healthy");

    send_frame(active_client.get(), request);
    expect_echo(active_client.get(), parser, request);

    std::cout
        << "[PASS] Incomplete frame times out despite trickled bytes; "
        << "server remains healthy\n";
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr
            << "Usage: server_integration_tests "
            << "./build/netforge_server\n";
        return 2;
    }

    try
    {
        {
            ServerProcess server(argv[1]);

            test_idle_client_does_not_block();
            test_two_frames_in_one_send();
            test_fragmented_frame();
            test_invalid_frame_does_not_stop_server();
            test_maximum_client_limit();

            // Stop the first server BEFORE launching another.
            server.stop();
        }

        {
            ServerProcess short_timeout_server(argv[1], "700");
            test_idle_client_times_out();
            short_timeout_server.stop();
        }

        {
            ServerProcess frame_timeout_server(argv[1],"4000","1500");
            test_incomplete_frame_times_out_despite_trickled_bytes();
            frame_timeout_server.stop();
        }

        std::cout << "\nAll integration tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "\nINTEGRATION TEST FAILED: " << exception.what() << '\n';
        return 1;
    }
}