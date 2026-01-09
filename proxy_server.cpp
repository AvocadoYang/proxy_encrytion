#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <openssl/err.h>
#include <spdlog/spdlog.h>

class Proxy_server
{
private:
    bool MODE;
    int create_socket()
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

        if (listen(s, 5) < 0)
        {
            spdlog::error("listing problem...");
            exit(EXIT_FAILURE);
        }

        return s;
    }

    SSL_CTX *create_context()
    {
        const SSL_METHOD *method;
        SSL_CTX *context;
        method = TLS_server_method();
        context = SSL_CTX_new(method);
        if (!context)
        {
            spdlog::error("context creation problem...");
            ERR_print_errors_fp(stderr);
            exit(EXIT_FAILURE);
        };
        if (SSL_CTX_use_certificate_file(context, "./security/server.crt", SSL_FILETYPE_PEM) <= 0)
        {
            spdlog::error("can't load certificate from file system...");
            exit(EXIT_FAILURE);
        };
        if (SSL_CTX_use_PrivateKey_file(context, "./security/server.key", SSL_FILETYPE_PEM) <= 0)
        {
            spdlog::error("can't load private key from file system...");
            exit(EXIT_FAILURE);
        };
        return context;
    }

public:
    int ep_fd;
    int listen_fd;
    SSL_CTX *context;
    Proxy_server(bool enable_tls)
    {
        context = enable_tls ? create_context() : nullptr;
        listen_fd = this->create_socket();
        this->MODE = enable_tls ? true : false;
        this->set_nonblocking(listen_fd);
        this->ep_fd = epoll_create1(0);

        if (this->add_epoll_event(listen_fd, EPOLL_CTL_ADD, EPOLLIN) < 0)
        {
            spdlog::error("add epoll event problem...");
            exit(EXIT_FAILURE);
        };
    }

    int add_epoll_event(int fd, int ep_ctl_op, uint32_t events)
    {

        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        return epoll_ctl(this->ep_fd, ep_ctl_op, fd, &ev);
    }

    void set_nonblocking(int fd)
    {
        int flags = fcntl(fd, F_GETFL, 0);
        int result = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (result < 0)
        {
            spdlog::error("fd control problem...");
            exit(EXIT_FAILURE);
        }
    }

    int handle_server_side(SSL *ssl, int client_fd, int server_fd)
    {
        char buffer[4096];
        int bytes;

        bytes = recv(server_fd, buffer, sizeof(buffer), 0);
        if (bytes <= 0)
        {
            printf("Server disconnect \n");
            return bytes;
        }

        printf("Received msg from server: %s \n", buffer);
        SSL_write(ssl, buffer, bytes);
        return bytes;
    }

    int handle_client_side(SSL *ssl, int client_fd, int server_fd)
    {
        char buffer[4096];
        int bytes;

        if (this->MODE)
        {
            bytes = SSL_read(ssl, buffer, sizeof(buffer) - 1);
        }
        else
        {
            bytes = recv(client_fd, buffer, sizeof(buffer), 0);
        }
        if (bytes <= 0)
        {
            printf("Client disconnect \n");
            return bytes;
        }
        printf("Received msg from client: %s \n", buffer);
        send(server_fd, buffer, bytes, 0);
        return bytes;
    }
};