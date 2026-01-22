
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

struct ProxyConnection
{
    int client_fd;
    int server_fd;
    SSL *ssl;
    bool ssl_accepted;
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

int start_server_connect(Proxy_server *, const ProxyConnection &);

void close_connection(const ProxyConnection *);

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    if (argc > 2)
    {
        perror("Get invalid arg num");
        exit(-1);
    }

    if (argc == 2 && strcmp("ssl", argv[1]))
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

                auto conn = std::make_unique<ProxyConnection>();
                conn->client_fd = client_fd;
                conn->server_fd = -1;
                conn->ssl = nullptr;
                conn->ssl_accepted = false;
                conn->server_connected = false;

                if (MODE == MODE_TLS)
                {
                    conn->ssl = SSL_new(server.context);
                    SSL_set_fd(conn->ssl, client_fd);
                    server.add_epoll_event(client_fd, EPOLL_CTL_ADD, EPOLLIN | EPOLLET);
                }
                else
                {
                    conn->ssl_accepted = true;
                    start_server_connect(&server, *conn);
                }

                // --------------- Connect to Server ---------------
                conns[client_fd] = std::move(conn);
            }
            else
            {
                ProxyConnection *conn = find_conn_by_fd(fd);
                if (!conn)
                    continue;
                printf("server_fd: %d, client_fd: %d, is_conn: %d \n", conn->server_fd, conn->client_fd, conn->server_connected);
                if (MODE == MODE_TLS && !conn->ssl_accepted)
                {
                    int ret = SSL_accept(conn->ssl);
                    if (ret < 0)
                    {
                        int err = SSL_get_error(conn->ssl, ret);
                        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                        {
                            continue;
                        }
                        else
                        {
                            spdlog::error("TLS Handshake failed");
                            close_connection(conn);
                            continue;
                        }
                    }
                    conn->ssl_accepted = true;
                    spdlog::info("TLS Handshake success");
                    int s_ret = start_server_connect(&server, *conn);

                    if (s_ret < 0)
                    {
                        spdlog::error("Proxy side not working");
                        close_connection(conn);
                        continue;
                    }
                    conn->server_fd = s_ret;
                    auto con = find_conn_by_fd(s_ret);
                }
                else if (fd == conn->server_fd && !conn->server_connected)
                {
                    int err;
                    socklen_t len = sizeof(err);
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                    if (err != 0)
                    {
                        spdlog::error("proxy error, disconnect socket with client");
                        close_connection(conn);
                        continue;
                    }

                    conn->server_connected = true;
                    if (server.add_epoll_event(conn->client_fd, EPOLL_CTL_MOD, EPOLLIN | EPOLLOUT) < 0 ||
                        server.add_epoll_event(conn->server_fd, EPOLL_CTL_MOD, EPOLLIN | EPOLLOUT) < 0)
                    {
                        perror("add epoll event problem... (create bridge)");
                        exit(EXIT_FAILURE);
                    }
                    spdlog::info("accept clinet connect, start proxy to server");
                }
                else if ((fd == conn->server_fd || fd == conn->client_fd) && conn->server_connected)
                {
                    int ret = -1;
                    if (fd == conn->client_fd)
                    {
                        ret = server.handle_client_side(conn->ssl, conn->client_fd, conn->server_fd);
                    }
                    else
                    {
                        ret = server.handle_server_side(conn->ssl, conn->client_fd, conn->server_fd);
                    }
                    if (ret <= 0)
                    {
                        close_connection(conn);
                    }
                }
            }
        }
    }
    return 0;
}

int start_server_connect(Proxy_server *server, const ProxyConnection &conn)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        SSL_shutdown(conn.ssl);
        SSL_free(conn.ssl);
        close(conn.client_fd);
        return server_fd;
    }
    server->set_nonblocking(server_fd);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(16665);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (server->add_epoll_event(server_fd, EPOLL_CTL_ADD, EPOLLIN) < 0)
    {
        spdlog::error("add epoll event problem...");
        exit(EXIT_FAILURE);
    };
    if (connect(server_fd, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        if (errno == EINPROGRESS)
        {
            printf("Connection in progress (EINPROGRESS)...\n");
            return server_fd;
        }
        else
        {

            perror("Real connect error");
            close(server_fd);
            return -1;
        }
    }
    return server_fd;
};

void close_connection(const ProxyConnection *conn)
{
    SSL_shutdown(conn->ssl);
    SSL_free(conn->ssl);
    close(conn->client_fd);
    if (conn->server_fd > 0)
    {
        close(conn->server_fd);
    }
    conns.erase(conn->client_fd);
}
