
#include <signal.h>
#include <iostream>
#include <fstream>
#include "./yaml.h"
#include "./type.hpp"
#include <typeinfo>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;
using namespace std;

std::map<int, std::unique_ptr<ProxyConnection>>
    conns;

ProxyMode MODE;

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    if (argc > 2)
    {
        perror("Get invalid arg num");
        exit(-1);
    }

    if (argc == 2 && strcmp("tls", argv[1]))
    {
        fprintf(stderr, "Get invalid arg \" %s \" \n", argv[1]);
        exit(EXIT_FAILURE);
    }
    if (argc == 2)
    {
        spdlog::info("start TLS mode \n");
        MODE = MODE_TLS;
    }
    else
    {
        spdlog::info("start Context mode \n");
        MODE = MODE_PLAN;
    }

    std::ifstream f("./config.json");
    if (!f)
    {
        cerr << "can't open config.json \n";
        exit(EXIT_FAILURE);
    }
    json j = json::parse(f);
    Config config = j.get<Config>();

    std::cout << "- Setting file (config.json):" << std::endl;
    std::cout << j.dump() << std::endl;

    Proxy_server server(config, MODE == MODE_TLS);

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
                int client_fd = accept(server.listen_fd, nullptr, nullptr);
                if (client_fd < 0)
                    continue;

                auto conn = std::make_unique<ProxyConnection>();
                conn->client_fd = client_fd;
                conn->server_fd = -1;
                conn->ssl = nullptr;
                conn->server_connected = false;
                conn->protocol_checked = false;

                server.set_nonblocking(client_fd);
                server.add_epoll_event(client_fd, EPOLL_CTL_ADD, EPOLLIN | EPOLLET);

                if (MODE == MODE_TLS)
                {
                    conn->ssl = SSL_new(server.context);
                    SSL_set_fd(conn->ssl, client_fd);
                }
                else
                {
                    Server_connect_res s_res = start_server_connect(&server, *conn, config);
                    printf("connect server response: c_ret - %d,  server_fd - %d \n", s_res.c_ret, s_res.server_fd);
                    if (s_res.c_ret < 0)
                    {
                        spdlog::error("Proxy side not working");
                        close_connection(conn.get());
                        continue;
                    }
                    conn->server_fd = s_res.server_fd;
                    conn->server_connected = true;
                }

                conns[client_fd] = std::move(conn);
            }
            else
            {
                ProxyConnection *conn = find_conn_by_fd(fd);
                if (!conn)
                    continue;
                if (fd == conn->client_fd && MODE == MODE_TLS && !conn->ssl_accepted)
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
                    Server_connect_res s_res = start_server_connect(&server, *conn, config);

                    if (s_res.c_ret < 0)
                    {
                        spdlog::error("Proxy side not working");
                        close_connection(conn);
                        continue;
                    }
                    conn->server_fd = s_res.server_fd;
                    conn->server_connected = true;
                }
                if (fd == conn->client_fd && !conn->protocol_checked)
                {
                    int ret = server.align_between_connection(
                        conn->client_fd,
                        MODE);

                    if (ret == -1)
                    {
                        spdlog::error("Client uses TLS but proxy is plaintext");
                        close_connection(conn);
                        continue;
                    }
                    if (ret == -2)
                    {
                        spdlog::error("Client is plaintext but proxy is TLS");
                        close_connection(conn);
                        continue;
                    }
                    if (ret == -3)
                    {
                        spdlog::info("Client closed connection");
                        close_connection(conn);
                        continue;
                    }

                    if (ret == 0)
                    {
                        conn->protocol_checked = true;
                    }
                    continue;
                }
                else if ((fd == conn->server_fd || fd == conn->client_fd) && conn->server_connected)
                {
                    int ret;

                    if (fd == conn->client_fd)
                    {
                        ret = server.handle_client_side(
                            conn->ssl,
                            conn->client_fd,
                            conn->server_fd);
                    }
                    else
                    {
                        ret = server.handle_server_side(
                            conn->ssl,
                            conn->client_fd,
                            conn->server_fd);
                    }

                    if (ret == 0)
                    {
                        close_connection(conn);
                    }
                    else if (ret < 0)
                    {
                        spdlog::error("proxy connection error, fd={}", fd);
                        close_connection(conn);
                    }
                }
            }
        }
    }
    return 0;
}

Server_connect_res start_server_connect(Proxy_server *server, const ProxyConnection &conn, Config config)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        Server_connect_res res;
        res.c_ret = -1;
        res.server_fd = -1;
        return res;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.proxy_pass);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int ret = connect(server_fd, (sockaddr *)&addr, sizeof(addr));
    if (ret >= 0)
    {
        printf("client_f: %d, server_f: %d \n", conn.client_fd, server_fd);
        server->set_nonblocking(server_fd);
        if (server->add_epoll_event(conn.client_fd, EPOLL_CTL_MOD, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLERR) < 0 ||
            server->add_epoll_event(server_fd, EPOLL_CTL_ADD, EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLERR) < 0)
        {
            perror("add epoll event problem... (create bridge)");
            exit(EXIT_FAILURE);
        }
        spdlog::info("accept clinet connect, start proxy to server");
    }
    else
    {
        close(server_fd);
    }
    Server_connect_res res;
    res.c_ret = ret;
    res.server_fd = server_fd;
    return res;
}

void close_connection(const ProxyConnection *conn)
{
    printf("close connect between %d and %d \n", conn->client_fd, conn->server_fd);
    close(conn->client_fd);
    if (conn->ssl != nullptr)
    {
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
    }
    if (conn->server_fd > 0)
    {
        close(conn->server_fd);
    }
    conns.erase(conn->client_fd);
}

ProxyConnection *find_conn_by_fd(int fd)
{
    for (auto &[_, conn] : conns)
    {
        if (conn->client_fd == fd || conn->server_fd == fd)
            return conn.get();
    }
    return nullptr;
}

void from_json(const json &j, Config &config)
{
    // Use .at() to access keys; it throws an exception if the key is missing
    j.at("path").get_to(config.path);
    j.at("server_listen").get_to(config.server_listen);
    j.at("proxy_pass").get_to(config.proxy_pass);
    // You can also use j.get<std::string>() or other types directly
}