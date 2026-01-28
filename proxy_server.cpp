#include "./type.hpp"

#include <stdio.h>
#include <iostream>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <spdlog/spdlog.h>

/* ================= private helpers ================= */

int Proxy_server::create_socket()
{
    int s;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(16666);
    addr.sin_addr.s_addr = INADDR_ANY;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
    {
        spdlog::error("socket creation problem...");
        exit(EXIT_FAILURE);
    }

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        spdlog::error("binding problem...");
        exit(EXIT_FAILURE);
    }

    if (listen(s, 128) < 0)
    {
        spdlog::error("listen problem...");
        exit(EXIT_FAILURE);
    }

    return s;
}

SSL_CTX *Proxy_server::create_context()
{
    const SSL_METHOD *method = TLS_server_method();
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx)
    {
        spdlog::error("SSL_CTX creation failed");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    if (SSL_CTX_use_certificate_file(ctx, "./security/server.crt", SSL_FILETYPE_PEM) <= 0)
    {
        spdlog::error("load certificate failed");
        exit(EXIT_FAILURE);
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, "./security/server.key", SSL_FILETYPE_PEM) <= 0)
    {
        spdlog::error("load private key failed");
        exit(EXIT_FAILURE);
    }

    return ctx;
}

/* ================= public methods ================= */

Proxy_server::Proxy_server(bool enable_tls)
    : ep_fd(-1),
      listen_fd(-1),
      context(nullptr),
      enable_tls_(enable_tls)
{
    if (enable_tls_)
        context = create_context();

    listen_fd = create_socket();
    set_nonblocking(listen_fd);

    ep_fd = epoll_create1(0);
    if (ep_fd < 0)
    {
        spdlog::error("epoll_create1 failed");
        exit(EXIT_FAILURE);
    }

    if (add_epoll_event(listen_fd, EPOLL_CTL_ADD, EPOLLIN) < 0)
    {
        spdlog::error("add epoll event failed");
        exit(EXIT_FAILURE);
    }
}

int Proxy_server::add_epoll_event(int fd, int op, uint32_t events)
{
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(ep_fd, op, fd, &ev);
}

void Proxy_server::set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        spdlog::error("fcntl nonblocking failed");
        exit(EXIT_FAILURE);
    }
}

/**
 * return:
 *   0   -> protocol matched
 *  -1   -> client TLS but proxy plain
 *  -2   -> client plain but proxy TLS
 */
int Proxy_server::align_between_connection(int client_fd, ProxyMode mode)
{
    unsigned char peek;
    int ret = recv(client_fd, &peek, 1, MSG_PEEK);
    if (ret < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return 0;
        }
        return -1;
    }

    bool client_tls = (ret == 1 && peek == 0x16);

    if (client_tls && mode == MODE_PLAN)
        return -1;

    if (!client_tls && mode == MODE_TLS)
        return -2;

    return 0;
}

int Proxy_server::handle_server_side(SSL *ssl, int client_fd, int server_fd)
{
    char buffer[4096];

    while (true)
    {
        int bytes = recv(server_fd, buffer, sizeof(buffer), 0);

        if (bytes > 0)
        {
            int sent = 0;
            while (sent < bytes)
            {
                int n;
                if (enable_tls_)
                {
                    n = SSL_write(ssl, buffer + sent, bytes - sent);
                    if (n <= 0)
                    {
                        int err = SSL_get_error(ssl, n);
                        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                            return 1;
                        return -1;
                    }
                }
                else
                {
                    n = send(client_fd, buffer + sent, bytes - sent, 0);
                    if (n < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            return 1;
                        return -1;
                    }
                }
                sent += n;
            }
        }
        else if (bytes == 0)
        {
            // backend server 關閉
            return 0;
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // 讀完了，等下一次 epoll
                return 1;
            }
            return -1;
        }
    }
}

int Proxy_server::handle_client_side(SSL *ssl, int client_fd, int server_fd)
{
    char buffer[4096];

    while (true)
    {
        int bytes;

        if (enable_tls_)
        {
            bytes = SSL_read(ssl, buffer, sizeof(buffer));
            if (bytes <= 0)
            {
                int err = SSL_get_error(ssl, bytes);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                    return 1;
                return -1;
            }
        }
        else
        {
            bytes = recv(client_fd, buffer, sizeof(buffer), 0);
            if (bytes <= 0)
            {
                if (bytes == 0)
                    return 0;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return 1;
                return -1;
            }
        }

        int sent = 0;
        while (sent < bytes)
        {
            int n = send(server_fd, buffer + sent, bytes - sent, 0);
            if (n < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return 1;
                return -1;
            }
            sent += n;
        }
    }
}
