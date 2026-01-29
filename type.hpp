#pragma once

#include <netinet/in.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

#include <openssl/ssl.h>

using json = nlohmann::json;
struct Server_connect_res
{
    int c_ret;
    int server_fd;
};

struct Config
{
    std::string path;
    int server_listen;
    int proxy_pass;
};

struct ProxyConnection
{
    int client_fd;
    int server_fd;
    SSL *ssl;
    bool ssl_accepted;
    bool server_connected;
    bool protocol_checked;
};

enum ProxyMode
{
    MODE_PLAN = 0,
    MODE_TLS = 1
};

class Proxy_server
{
private:
    bool enable_tls_;

    int create_socket();
    SSL_CTX *create_context();

public:
    int ep_fd;
    int listen_fd;
    SSL_CTX *context;
    std::string cert_path;
    int proxy_server_ip;

    explicit Proxy_server(Config config, bool enable_tls);

    int add_epoll_event(int fd, int ep_ctl_op, uint32_t events);

    int align_between_connection(int client_fd, ProxyMode MODE);

    void set_nonblocking(int fd);

    int handle_server_side(SSL *ssl, int client_fd, int server_fd);
    int handle_client_side(SSL *ssl, int client_fd, int server_fd);
};

Server_connect_res start_server_connect(Proxy_server *, const ProxyConnection &, Config);

void close_connection(const ProxyConnection *);

ProxyConnection *find_conn_by_fd(int);

void from_json(const json &, Config &);
