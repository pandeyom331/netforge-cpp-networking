
#include <arpa/inet.h>
#include <cerrno>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

constexpr int PORT = 54000;
constexpr std::size_t BUFFER_SIZE = 1024;

bool send_all(int fd, const char* data, std::size_t length)
{
    std::size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd, data + sent,
                         length - sent, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR)
                continue;

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

int main()
{
    std::string message;

    std::cout << "Enter a message: ";
    if (!std::getline(std::cin, message)) {
        std::cerr << "Failed to read input\n";
        return 1;
    }

    // Our simple protocol uses newline to end a message.
    message.push_back('\n');

    // 1. Create a TCP socket.
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (client_fd == -1) {
        perror("socket");
        return 1;
    }

    // 2. Specify the server address.
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, "127.0.0.1",
                  &server_addr.sin_addr) != 1) {
        std::cerr << "Invalid server IP address\n";
        close(client_fd);
        return 1;
    }

    // 3. Establish the TCP connection.
    if (connect(client_fd,
                reinterpret_cast<sockaddr*>(&server_addr),
                sizeof(server_addr)) == -1) {
        perror("connect");
        close(client_fd);
        return 1;
    }

    std::cout << "Connected to server\n";

    // 4. Send the message.
    if (!send_all(client_fd, message.data(), message.size())) {
        close(client_fd);
        return 1;
    }

    // 5. Receive the echoed message.
    std::string response;
    char buffer[BUFFER_SIZE];
    bool complete = false;

    while (!complete) {
        ssize_t n = recv(client_fd, buffer,
                         sizeof(buffer), 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            perror("recv");
            close(client_fd);
            return 1;
        }

        if (n == 0)
            break;

        response.append(buffer, static_cast<std::size_t>(n));

        if (response.find('\n') != std::string::npos)
            complete = true;
    }

    close(client_fd);

    if (!complete) {
        std::cerr << "Connection closed before the full response\n";
        return 1;
    }

    std::cout << "Echo received: " << response;
    return 0;
}