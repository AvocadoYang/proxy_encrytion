
#include <signal.h>
#include <iostream>
#include <fstream>
#include "./yaml.h"
#include "./proxy_server.cpp"
#include <typeinfo>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;
using namespace std;

const char *ssl = "ssl";

struct ProxyConnection
{
    int client_fd;
    int server_fd;
    SSL *ssl;
    bool server_connected;
    time_t connect_start;
};

enum ProxyMode
{
    MODE_PLAN = 0,
    MODE_TLS = 1
};

ProxyMode MODE;

struct Config
{
    int server_listen;
    int proxy_pass;
};

std::map<int, std::unique_ptr<ProxyConnection>>
    conns;

ProxyConnection *find_conn_by_fd(int fd)
{
    for (auto &[_, conn] : conns)
    {
        if (conn->client_fd == fd || conn->server_fd == fd)
            return conn.get();
    }
    return nullptr;
}

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    if (argc > 2)
    {
        perror("Get invalid arg num");
        exit(-1);
    }

    if (argc == 2 && strcmp(ssl, argv[1]))
    {
        fprintf(stderr, "Get invalid arg [ \" %s \" ]\n", argv[1]);
        exit(EXIT_FAILURE);
    }
    if (argc == 2)
    {
        spdlog::info("start SSL mode \n");
        MODE = MODE_TLS;
    }
    else
    {
        spdlog::info("start Context mode \n");
        MODE = MODE_PLAN;
    }

    // std::cout << argv[1] << std::endl;

    std::ifstream f("./config.json");
    if (!f)
    {
        cerr << "can't open config.json \n";
        exit(EXIT_FAILURE);
    }
    printf("- Setting file (config.json): \n");
    string line;

    while (getline(f, line))
    {
        cout << line << endl;
    }

    Proxy_server server(MODE == MODE_TLS);

    epoll_event events[1024];
    while (true)
    {
        int n = epoll_wait(server.ep_fd, events, 1024, -1);

        // time_t now = time(nullptr);
        // for (auto it = conns.begin(); it != conns.end();)
        // {
        //     auto &c = it->second;
        //     if (!c->server_connected && (now - c->connect_start) > 3)
        //     {
        //         printf("%ld !>#!>#!# \n", now - c->connect_start);
        //         close(c->client_fd);
        //         close(c->server_fd);
        //         printf("close bridge \n");
        //         SSL_free(c->ssl);
        //         printf("@@@@??");
        //         it = conns.erase(it);
        //     }
        //     else
        //     {
        //         ++it;
        //     }
        // }

        for (int i = 0; i < n; ++i)
        {
            int fd = events[i].data.fd;
            if (fd == server.listen_fd)
            {
                // --------------- Accept from Client ---------------
                sockaddr_in client_addr{};
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server.listen_fd, (sockaddr *)&client_addr, &client_len);
                if (client_fd < 0)
                    continue;

                SSL *ssl = nullptr;

                if (MODE == MODE_TLS)
                {
                    ssl = SSL_new(server.context);
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
                }

                // --------------- Connect to Server ---------------

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
                addr.sin_port = htons(16665);
                inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
                connect(server_fd, (sockaddr *)&addr, sizeof(addr));

                server.add_epoll_event(server_fd, EPOLL_CTL_ADD, EPOLLOUT);

                auto conn = std::make_unique<ProxyConnection>();
                conn->client_fd = client_fd;
                conn->server_fd = server_fd;
                conn->ssl = ssl;
                conn->server_connected = false;
                conn->connect_start = time(nullptr);

                conns[client_fd] = std::move(conn);

                printf("accept clinet connect, start proxy to server \n");
            }
            else
            {
                ProxyConnection *conn = find_conn_by_fd(fd);
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
                        conns.erase(conn->client_fd);
                        printf("proxy error, disconnect socket with client \n");
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
                    int ret = 1;
                    if (fd == conn->client_fd)
                    {
                        ret = server.handle_client_side(conn->ssl, conn->client_fd, conn->server_fd);
                    }
                    else
                    {
                        ret = server.handle_server_side(conn->ssl, conn->client_fd, conn->server_fd);
                        printf("%d \n", ret);
                    }
                    if (ret <= 0)
                    {
                        close(conn->client_fd);
                        close(conn->server_fd);
                        SSL_free(conn->ssl);
                        conns.erase(conn->client_fd);
                    }
                }
            }
        }
    }
    return 0;
}