
#include "netforge/frame_parser.hpp"
#include "netforge/socket_handle.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <utility>

namespace {

constexpr std::uint16_t PORT = 54000;
constexpr std::size_t RECEIVE_CHUNK_SIZE = 4096;

bool send_all(
    int fd,
    const std::uint8_t* data,
    std::size_t size)
{
    std::size_t sent = 0;

    while (sent < size) {
        const ssize_t n = send(
            fd,
            data + sent,
            size - sent,
            MSG_NOSIGNAL
        );

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("send");
            return false;
        }

        if (n == 0) {
            std::cerr << "send returned zero\n";
            return false;
        }

        sent += static_cast<std::size_t>(n);
    }

    return true;
}

bool receive_frame(
    int fd,
    netforge::FrameParser& parser,
    netforge::Frame& frame)
{
    std::uint8_t buffer[RECEIVE_CHUNK_SIZE];

    while (true) {
        auto result = parser.next_frame();

        if (result.status == netforge::DecodeStatus::Complete) {
            frame = std::move(result.frame);
            return true;
        }

        if (result.status == netforge::DecodeStatus::Invalid) {
            std::cerr << "Invalid protocol frame: "
                      << result.error << '\n';
            return false;
        }

        const ssize_t n = recv(
            fd,
            buffer,
            sizeof(buffer),
            0
        );

        if (n == 0) {
            std::cerr << "Server disconnected\n";
            return false;
        }

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("recv");
            return false;
        }

        std::string error;

        if (!parser.append(
                buffer,
                static_cast<std::size_t>(n),
                error)) {
            std::cerr << "Could not buffer input: "
                      << error << '\n';
            return false;
        }
    }
}

} // namespace

int main()
{
    using netforge::SocketHandle;

    SocketHandle client_fd(
        ::socket(AF_INET, SOCK_STREAM, 0));

    if (!client_fd.valid()) {
        ::perror("socket");
        return 1;
    }

    sockaddr_in server_address{};

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(54000);

    if (::inet_pton(
            AF_INET,
            "127.0.0.1",
            &server_address.sin_addr) != 1) {
        std::cerr << "Invalid server address\n";
        return 1;
    }

    if (::connect(
            client_fd.get(),
            reinterpret_cast<sockaddr*>(&server_address),
            sizeof(server_address)) == -1) {
        ::perror("connect");
        return 1;
    }

    std::cout << "Connected to NetForge server\n";

    netforge::FrameParser parser;
    std::uint32_t next_request_id = 1;

    while (true) {
        std::string message;

        std::cout << "Message (/quit to exit): ";

        if (!std::getline(std::cin, message)) {
            break;
        }

        if (message == "/quit") {
            break;
        }

        if (message.size() > netforge::MAX_PAYLOAD_SIZE) {
            std::cerr << "Message exceeds the maximum payload size\n";
            continue;
        }

        netforge::Frame request;
        request.type = netforge::MessageType::Chat;
        request.request_id = next_request_id++;
        request.payload.assign(message.begin(), message.end());

        const auto encoded = netforge::encode_frame(request);

        if (!send_all(
                client_fd.get(),
                encoded.data(),
                encoded.size())) {
            return 1;
        }

        netforge::Frame response;

        if (!receive_frame(client_fd.get(), parser, response)) {
            return 1;
        }

        if (response.type != netforge::MessageType::Chat ||
            response.request_id != request.request_id) {
            std::cerr << "Unexpected response frame\n";
            return 1;
        }

        std::cout << "Echo received: ";

        if (!response.payload.empty()) {
            std::cout.write(
                reinterpret_cast<const char*>(
                    response.payload.data()),
                static_cast<std::streamsize>(
                    response.payload.size()));
        }

        std::cout << '\n';
    }

    std::cout << "Disconnected from NetForge server\n";
    return 0;
}