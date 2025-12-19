#include <iostream>
#include <vector>
#include <unordered_map>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <cstring>

constexpr int PROXY_PORT = 8000;
constexpr int SERVER_PORT = 9000;
constexpr int BUFFER_SIZE = 4096;

void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// ---------------- ProxyClient ----------------
class ProxyClient
{
public:
    int fd;
    std::vector<char> send_buffer;

    ProxyClient(int client_fd) : fd(client_fd)
    {
        set_nonblocking(fd);
    }

    ~ProxyClient()
    {
        close(fd);
    }

    int recv_from_client(std::vector<char> &c2s_buffer)
    {
        char buf[BUFFER_SIZE];
        int n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0)
        {
            c2s_buffer.insert(c2s_buffer.end(), buf, buf + n);
        }
        return n;
    }

    int send_to_client()
    {
        if (send_buffer.empty())
            return 0;
        int n = send(fd, send_buffer.data(), send_buffer.size(), 0);
        if (n > 0)
            send_buffer.erase(send_buffer.begin(), send_buffer.begin() + n);
        return n;
    }
};

// ---------------- ProxyServer ----------------
class ProxyServer
{
public:
    int fd;
    std::vector<char> send_buffer;

    ProxyServer()
    {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        set_nonblocking(fd);

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

        if (connect(fd, (sockaddr *)&server_addr, sizeof(server_addr)) < 0)
        {
            if (errno != EINPROGRESS)
            {
                perror("connect");
                close(fd);
                fd = -1;
            }
        }
    }

    ~ProxyServer()
    {
        if (fd >= 0)
            close(fd);
    }

    int recv_from_server(std::vector<char> &s2c_buffer)
    {
        char buf[BUFFER_SIZE];
        int n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0)
            s2c_buffer.insert(s2c_buffer.end(), buf, buf + n);
        return n;
    }

    int send_to_server()
    {
        if (send_buffer.empty())
            return 0;
        int n = send(fd, send_buffer.data(), send_buffer.size(), 0);
        if (n > 0)
            send_buffer.erase(send_buffer.begin(), send_buffer.begin() + n);
        return n;
    }
};

// ---------------- Connection ----------------
struct Connection
{
    ProxyClient client;
    ProxyServer server;
    std::vector<char> c2s_buffer; // client->server
    std::vector<char> s2c_buffer; // server->client

    Connection(int client_fd) : client(client_fd), server() {}
};

int main()
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    set_nonblocking(listen_fd);

    sockaddr_in proxy_addr{};
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_addr.s_addr = INADDR_ANY;
    proxy_addr.sin_port = htons(PROXY_PORT);

    bind(listen_fd, (sockaddr *)&proxy_addr, sizeof(proxy_addr));
    listen(listen_fd, 128);

    std::cout << "Proxy listening on port " << PROXY_PORT << std::endl;

    std::unordered_map<int, Connection> connections;

    fd_set readfds, writefds;
    int max_fd;

    while (true)
    {
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        FD_SET(listen_fd, &readfds);
        max_fd = listen_fd;

        for (auto &[fd, conn] : connections)
        {
            FD_SET(conn.client.fd, &readfds);
            FD_SET(conn.server.fd, &readfds);

            if (!conn.client.send_buffer.empty())
                FD_SET(conn.client.fd, &writefds);
            if (!conn.server.send_buffer.empty())
                FD_SET(conn.server.fd, &writefds);

            if (conn.client.fd > max_fd)
                max_fd = conn.client.fd;
            if (conn.server.fd > max_fd)
                max_fd = conn.server.fd;
        }

        int ret = select(max_fd + 1, &readfds, &writefds, nullptr, nullptr);
        if (ret < 0)
        {
            perror("select");
            break;
        }

        // accept new client
        if (FD_ISSET(listen_fd, &readfds))
        {
            int client_fd = accept(listen_fd, nullptr, nullptr);
            if (client_fd >= 0)
            {
                connections[client_fd] = Connection(client_fd);
                std::cout << "New connection: client_fd=" << client_fd
                          << " server_fd=" << connections[client_fd].server.fd << std::endl;
            }
        }

        std::vector<int> to_remove;
        for (auto &[fd, conn] : connections)
        {
            // client -> server
            if (FD_ISSET(conn.client.fd, &readfds))
            {
                int n = conn.client.recv_from_client(conn.c2s_buffer);
                if (n <= 0)
                    to_remove.push_back(fd);
            }

            // server -> client
            if (FD_ISSET(conn.server.fd, &readfds))
            {
                int n = conn.server.recv_from_server(conn.s2c_buffer);
                if (n <= 0)
                    to_remove.push_back(fd);
            }

            // write to server
            if (FD_ISSET(conn.server.fd, &writefds) && !conn.c2s_buffer.empty())
            {
                int n = send(conn.server.fd, conn.c2s_buffer.data(), conn.c2s_buffer.size(), 0);
                if (n > 0)
                    conn.c2s_buffer.erase(conn.c2s_buffer.begin(), conn.c2s_buffer.begin() + n);
            }

            // write to client
            if (FD_ISSET(conn.client.fd, &writefds) && !conn.s2c_buffer.empty())
            {
                int n = send(conn.client.fd, conn.s2c_buffer.data(), conn.s2c_buffer.size(), 0);
                if (n > 0)
                    conn.s2c_buffer.erase(conn.s2c_buffer.begin(), conn.s2c_buffer.begin() + n);
            }
        }

        // cleanup closed connections
        for (int fd : to_remove)
        {
            close(connections[fd].client.fd);
            close(connections[fd].server.fd);
            connections.erase(fd);
            std::cout << "Connection closed: " << fd << std::endl;
        }
    }

    close(listen_fd);
    return 0;
}
