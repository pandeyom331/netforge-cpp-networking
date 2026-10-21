
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

constexpr int PORT = 54000;
constexpr int BACKLOG = 10;
constexpr std::size_t BUFFER_SIZE = 1024;

// Send every byte, handling partial sends.
bool send_all(int fd, const char* data, std::size_t length)
{
    std::size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, MSG_NOSIGNAL);

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
    // 1. Create a TCP socket.
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd == -1) {
        perror("socket");
        return 1;
    }

    // Allow the address/port to be reused after restarting the server.
    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse, sizeof(reuse)) == -1) {
        perror("setsockopt");
        close(server_fd);
        return 1;
    }

    // 2. Configure the server address.
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // 3. Bind the socket to 127.0.0.1:54000.
    if (bind(server_fd,
             reinterpret_cast<sockaddr*>(&server_addr),
             sizeof(server_addr)) == -1) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    // 4. Start listening for incoming TCP connections.
    if (listen(server_fd, BACKLOG) == -1) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    std::cout << "Server listening on 127.0.0.1:"
              << PORT << '\n';

    // 5. Accept clients one at a time.
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(
            server_fd,
            reinterpret_cast<sockaddr*>(&client_addr),
            &client_len
        );

        if (client_fd == -1) {
            if (errno == EINTR)
                continue;

            perror("accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &client_addr.sin_addr,
                  client_ip, sizeof(client_ip));

        std::cout << "Client connected: "
                  << client_ip << '\n';

        // 6. Receive bytes and echo them back.
        char buffer[BUFFER_SIZE];

        while (true) {
            ssize_t n = recv(client_fd, buffer,
                             sizeof(buffer), 0);

            if (n == 0) {
                std::cout << "Client disconnected\n";
                break;
            }

            if (n < 0) {
                if (errno == EINTR)
                    continue;

                perror("recv");
                break;
            }

            if (!send_all(client_fd, buffer,
                          static_cast<std::size_t>(n))) {
                break;
            }

            // This demo handles one newline-terminated message
            // per connection.
            if (std::memchr(buffer, '\n',
                            static_cast<std::size_t>(n))) {
                break;
            }
        }

        close(client_fd);
    }

    close(server_fd);
    return 0;
}