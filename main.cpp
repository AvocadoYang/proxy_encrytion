#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

int main()
{
    int c_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8000);

    bind(c_fd, (sockaddr *)&addr, sizeof(addr));
    listen(c_fd, 5);

    std::cout << "Proxy listening on 8000\n";

    int client_fd = accept(c_fd, nullptr, nullptr);
    std::cout << "Client connected\n";

    //--------------------------

    int s_fd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(9000);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    connect(s_fd, (sockaddr *)&server_addr, sizeof(server_addr));
    std::cout << "Connected to backend server\n";

    char buf[4096];
    while (true)
    {
        int n = recv(client_fd, buf, sizeof(buf), 0);
        std::cout << n << std::endl;
        if (n <= 0)
            break;

        send(s_fd, buf, n, 0);

        n = recv(s_fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;

        send(client_fd, buf, n, 0);
    }

    close(client_fd);
    close(s_fd);

    return 0;
}