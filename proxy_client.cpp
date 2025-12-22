#include <sys/socket.h>
#include <arpa/inet.h>

class Proxy_client
{
private:
    sockaddr_in server_addr{};

public:
    int client_fd;
    Proxy_client()
    {
        this->server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(9000);
        inet_pton(AF_INET, "127.0.0.1", &(this->server_addr));
    }
};