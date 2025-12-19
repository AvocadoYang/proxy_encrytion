#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

int port = 8000;

int create_socket()
{
    int s;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
    {
        perror("socket creation problem...");
        exit(EXIT_FAILURE);
    }

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("binding problem...");
        exit(EXIT_FAILURE);
    }

    if (listen(s, 5) < 0)
    {
        perror("listing problem...");
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
        perror("context creation problem...");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    };
    if (SSL_CTX_use_certificate_file(context, "./security/server.crt", SSL_FILETYPE_PEM) <= 0)
    {
        perror("can't load certificate from file system...");
        exit(EXIT_FAILURE);
    };
    if (SSL_CTX_use_PrivateKey_file(context, "./security/server.key", SSL_FILETYPE_PEM) <= 0)
    {
        perror("can't load private key from file system...");
        exit(EXIT_FAILURE);
    };
    return context;
}

void handle_client(SSL *ssl, int client)
{
    char buffer[4096];
    int bytes;

    const char message[] = "Welcome to SSL/TCP server";

    SSL_write(ssl, message, strlen(message));

    while (1)
    {
        bytes = SSL_read(ssl, buffer, sizeof(buffer) - 1);
        if (bytes <= 0)
        {
            printf("Client disconnect \n");
            break;
        }

        printf("Received: %s\n", buffer);
        SSL_write(ssl, buffer, bytes);
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(client);
}

int main(int argc, char **argv)
{
    int sock;
    SSL_CTX *context;

    signal(SIGPIPE, SIG_IGN);

    sock = create_socket();
    context = create_context();

    // ************************** */

    struct sockaddr_in addr;
    unsigned int length = sizeof(addr);

    SSL *ssl;
    printf("waiting..... \n");

    int client = accept(sock, (sockaddr *)&addr, &length);

    if (client < 0)
    {
        perror("can't connect a client...");
        exit(EXIT_FAILURE);
    };

    ssl = SSL_new(context);
    SSL_set_fd(ssl, client);

    if (SSL_accept(ssl) <= 0)
    {
        ERR_print_errors_fp(stderr);
    }

    handle_client(ssl, client);
    /**************************** */

    return 0;
}