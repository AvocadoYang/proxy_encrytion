#pragma once

#include <netinet/in.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <memory>

#include <openssl/ssl.h>

struct ProxyConnection
{
    int client_fd;
    int server_fd;
    SSL *ssl;
    bool ssl_accepted;
    bool server_connected;
    bool algin_connect;
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

    explicit Proxy_server(bool enable_tls);

    int add_epoll_event(int fd, int ep_ctl_op, uint32_t events);

    int align_between_connection(int client_fd, ProxyMode MODE);

    void set_nonblocking(int fd);

    int handle_server_side(SSL *ssl, int client_fd, int server_fd);
    int handle_client_side(SSL *ssl, int client_fd, int server_fd);
};

struct Config
{
    int server_listen;
    int proxy_pass;
};

int start_server_connect(Proxy_server *, const ProxyConnection &);

void close_connection(const ProxyConnection *);

ProxyConnection *find_conn_by_fd(int);
