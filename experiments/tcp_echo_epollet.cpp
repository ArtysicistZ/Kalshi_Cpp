#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

int main() {

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int yes = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        perror("setsockopt");
        close(listen_fd);
        return 1;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = (uint16_t)htons(9000);
    addr.sin_addr.s_addr = (uint32_t)htonl(INADDR_ANY);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, 128) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create");
        close(listen_fd);
        return 1;
    }
    
    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        perror("epoll_ctl");
        close(listen_fd);
        close(epfd);
        return 1;
    }

    while (true) {
        struct epoll_event events[64];
        int n = epoll_wait(epfd, events, 64, -1);
        if (n < 0) {
            perror("epoll_wait");
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == listen_fd) {
                while (true) {
                    int client_fd = accept(listen_fd, nullptr, nullptr);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept");
                        continue;
                    }
                    if (fcntl(client_fd, F_SETFL, O_NONBLOCK) < 0) {
                        perror("fcntl");
                        close(client_fd);
                        continue;
                    }
                    struct epoll_event ev2{};
                    ev2.events = EPOLLIN | EPOLLET;
                    ev2.data.fd = client_fd;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev2) < 0) {
                        perror("epoll_ctl");
                        close(client_fd);
                        continue;
                    }
                }
                
            } else {
                char buf[4096];
                ssize_t n;
                while (true) {
                    n = read(events[i].data.fd, buf, sizeof(buf));
                    if (n > 0) {
                        ssize_t to_send = n;
                        ssize_t sent = 0;
                        while (sent < to_send) {
                            ssize_t n = write(events[i].data.fd, buf + sent, to_send - sent);
                            if (n < 0) {
                                perror("write");
                                break;
                            }
                            sent += n;
                        }
                    } else {
                        if (n < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            perror("read");
                        }
                        if (epoll_ctl(epfd, EPOLL_CTL_DEL, events[i].data.fd, nullptr) < 0) {
                            perror("epoll_ctl");
                            continue;
                        }
                        close(events[i].data.fd);
                        break;
                    }
                }
            }
        }

    }
    close(epfd);
    close(listen_fd);
    return 0;

}