#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int main(int argc, char** argv) {

    if (argc < 3) {
        fprintf(stderr, "too few arguement!");
        return 1;
    }
    char* host = argv[1];
    char* path = argv[2];

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    int rc = getaddrinfo(host, "80", &hints, &result);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return 1;
    }

    int fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd < 0) {
        perror("socket");
        freeaddrinfo(result);
        return 1;
    }
    if (connect(fd, result->ai_addr, result->ai_addrlen) < 0) {
        perror("connect");
        freeaddrinfo(result);
        close(fd);
        return 1;
    }
    freeaddrinfo(result);

    char request[1024];
    snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, host);

    size_t to_send = strlen(request);
    size_t sent = 0;
    while (sent < to_send) {
        ssize_t n = write(fd, request + sent, to_send - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("write");
            close(fd);
            return 1;
        }
        sent += n;
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, n, stdout);
    }
    if (n < 0) {
        perror("read");
        close(fd);
        return 1;
    }
    close(fd);

    return 0;

}