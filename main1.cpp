
#include <signal.h>
#include <iostream>
#include <fstream>
#include "./yaml.h"
#include "./proxy_server.cpp"
#include <typeinfo>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct ProxyConnection
{
    int client_fd;
    int server_fd;
    SSL *ssl;
    bool server_connected;
    time_t connect_start;
};
std::map<int, ProxyConnection *> conns;

int main()
{
    signal(SIGPIPE, SIG_IGN);

    std::ifstream f("./config.json");

    Proxy_server server;

    epoll_event events[1024];
    while (true)
    {
        int n = epoll_wait(server.ep_fd, events, 1024, 100);

        time_t now = time(nullptr);

        for (auto it = conns.begin(); it != conns.end();)
        {
            ProxyConnection *c = it->second;
            if (!c->server_connected && now - c->connect_start > 3)
            {
                printf("close bridge \n");
                close(c->client_fd);
                close(c->server_fd);
                SSL_free(c->ssl);
                delete c;
                it = conns.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (int i = 0; i < n; ++i)
        {
            int fd = events[i].data.fd;
            if (fd == server.listen_fd)
            {
                // --------------- Accept Client ---------------
                sockaddr_in client_addr{};
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server.listen_fd, (sockaddr *)&client_addr, &client_len);
                printf("%d #### \n", client_fd);
                if (client_fd < 0)
                    continue;

                SSL *ssl = SSL_new(server.context);
                SSL_set_fd(ssl, client_fd);

                int ret = SSL_accept(ssl);

                if (ret <= 0)
                {
                    int err = SSL_get_error(ssl, ret);
                    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                    {
                        continue;
                    }
                    else
                    {
                        ERR_print_errors_fp(stderr);
                        SSL_free(ssl);
                        close(client_fd);
                        continue;
                    }
                }

                // --------------- Connect Server ---------------

                int server_fd = socket(AF_INET, SOCK_STREAM, 0);
                if (server_fd < 0)
                {
                    SSL_shutdown(ssl);
                    SSL_free(ssl);
                    close(client_fd);
                    continue;
                }
                server.set_nonblocking(server_fd);
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(9000);
                inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
                connect(server_fd, (sockaddr *)&addr, sizeof(addr));

                server.add_epoll_event(server_fd, EPOLL_CTL_ADD, EPOLLOUT);

                ProxyConnection *conn = new ProxyConnection{
                    client_fd,
                    server_fd,
                    ssl,
                    false,
                    time(nullptr)};
                conns[client_fd] = conn;
                conns[server_fd] = conn;
            }
            else
            {
                ProxyConnection *conn = conns[fd];

                if (fd == conn->server_fd && !conn->server_connected)
                {
                    int err;
                    socklen_t len = sizeof(err);
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                    if (err != 0)
                    {
                        close(conn->client_fd);
                        close(conn->server_fd);
                        SSL_free(conn->ssl);
                        delete conn;
                        conns.erase(fd);
                        continue;
                    }

                    conn->server_connected = true;
                    if (server.add_epoll_event(conn->client_fd, EPOLL_CTL_ADD, EPOLLIN) < 0 ||
                        server.add_epoll_event(conn->server_fd, EPOLL_CTL_MOD, EPOLLIN) < 0)
                    {
                        perror("add epoll event problem... (create bridge)");
                        exit(EXIT_FAILURE);
                    }
                }
                else if ((fd == conn->server_fd || fd == conn->client_fd) && conn->server_connected)
                {
                    if (fd == conn->client_fd)
                    {
                        server.handle_tls_side(conn->ssl, conn->client_fd, conn->server_fd);
                    }
                    else
                    {
                        server.handle_tcp_side(conn->ssl, conn->client_fd, conn->server_fd);
                    }
                }
            }
        }
    }
    return 0;
}