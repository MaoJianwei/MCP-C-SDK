#include <errno.h>
#include <stdio.h>

#include <sys/epoll.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <string.h>


#define MAX_EVENTS 1024  // 最大监听事件数

// 定义日志级别
enum LOG_LEVEL {
    OFF = 0,
    FATAL,
    ERR,
    WARN,
    INFO = 10,
    DEBUG = 20
};

// 定义日志宏
#define LOG(level, format, ...) \
    if (level <= current_log_level) { \
        printf("[%d] " format "\n", level, ##__VA_ARGS__); \
    }

// 当前日志级别
static enum LOG_LEVEL current_log_level = DEBUG;
// static const char * log_level_strs[] = {
//     "OFF",
//
// }


int create_nonblocking_server_socket() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        LOG(ERR, "Fail to socket: errno %d", errno);
        return -1;
    }

    // 设置为非阻塞模式
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        LOG(ERR, "Fail to fcntl: errno %d", errno);
        close(sockfd);
        return -2;
    }

    // 设置socket选项，允许重用端口和地址
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        LOG(ERR, "Fail to setsockopt: errno %d", errno);
        return -3;
    }

    return sockfd;
}

int register_server_epoll_fd(int server_socket_fd) {
    int epoll_fd = epoll_create1(0);  // 创建epoll实例
    if (epoll_fd == -1) {
        LOG(ERR, "Fail to epoll_create1: errno %d", errno);
        return -1;
    }

    struct epoll_event ev;
    // 监听服务器Socket的"可读"事件（用于接收新连接）
    ev.events = EPOLLIN;  // 表示关注"有数据可读"事件
    ev.data.fd = server_socket_fd;  // 关联服务器Socket
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket_fd, &ev) == -1) {
        LOG(ERR, "Fail to epoll_ctl: server_socket_fd %d, errno: %d", server_socket_fd, errno);
        return -1;
    }

    return epoll_fd;
}

int start_server(int server_socket_fd, unsigned short port) {
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;  // 监听所有网络接口
    address.sin_port = htons(port);

    // 绑定socket到指定端口
    if (bind(server_socket_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        LOG(ERR, "Fail to bind, errno: %d", errno);
        return -1;
    }

    // 开始监听，最大等待连接数为5
    if (listen(server_socket_fd, 10) < 0) {
        LOG(ERR, "Fail to listen, errno: %d", errno);
        return -2;
    }

    LOG(INFO, "HTTP server listening on port %d...", port);
    return 0;
}

int epoll_loop(int server_socket_fd, int epoll_fd) {
    struct epoll_event events[MAX_EVENTS];
    while (1) {
        // 等待事件发生，超时时间设为-1表示永久阻塞
        int num_active_fds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (num_active_fds == -1) {
            LOG(ERR, "Fail to epoll_wait: errno %d", errno);
            break;
        }

        // 遍历所有触发的事件
        for (int i = 0; i < num_active_fds; i++) {
            if (events[i].data.fd == server_socket_fd) {
                // 服务器Socket有新连接（EPOLLIN事件）
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_socket_fd = accept(server_socket_fd, (struct sockaddr*)&client_addr, &client_len);
                if (client_socket_fd == -1) {
                    LOG(ERR, "Fail to accept: errno %d", errno);
                    continue;
                }

                LOG(INFO, "Accept: %d", client_socket_fd);

                // 将客户端Socket设为非阻塞
                int flags = fcntl(client_socket_fd, F_GETFL, 0);
                if (fcntl(client_socket_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
                    LOG(ERR, "Fail to fcntl: errno %d", errno);
                    close(client_socket_fd);
                    continue;
                }

                LOG(INFO, "Set Unblocking: client_socket_fd %d", client_socket_fd);

                // 注册客户端Socket的"可读"事件（等待数据）
                struct epoll_event client_ev;
                client_ev.events = EPOLLIN | EPOLLET;  // EPOLLET表示边缘触发（高效）
                client_ev.data.fd = client_socket_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket_fd, &client_ev) == -1) {
                    LOG(ERR, "Fail to epoll_ctl 1: errno %d", errno);
                    close(client_socket_fd);
                    continue;
                }

                LOG(INFO, "Add Epoll: client_socket_fd %d", client_socket_fd);

            } else if (events[i].events & EPOLLIN) {
                // 客户端Socket有数据可读
                int client_socket_fd = events[i].data.fd;
                char buf[10240];
                ssize_t n = read(client_socket_fd, buf, sizeof(buf)-1);
                if (n <= 0) {
                    // 连接关闭或错误，移除事件并关闭Socket
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_socket_fd, NULL);
                    close(client_socket_fd);
                } else {
                    buf[n] = '\0';
                    LOG(INFO, "Received: %s", buf);
                    // 可注册"可写"事件，准备发送响应
                    struct epoll_event ev;
                    ev.events = EPOLLOUT | EPOLLET;
                    ev.data.fd = client_socket_fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_socket_fd, &ev) == -1) {
                        LOG(ERR, "Fail to epoll_ctl 2: errno %d", errno);
                    }
                }
            } else if (events[i].events & EPOLLOUT) {
                // 客户端Socket可写（发送响应）
                int client_socket_fd = events[i].data.fd;
                const char* resp = "HTTP/1.1 200 OK\r\n";
                ssize_t n = send(client_socket_fd, resp, strlen(resp), 0);
                LOG(INFO, "Sent: %ld bytes", n);


                // 发送完成后，重新注册"可读"事件等待新数据
                struct epoll_event ev;
                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = client_socket_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_socket_fd, &ev) == -1) {
                    LOG(ERR, "Fail to epoll_ctl 3: errno %d", errno);
                }

                LOG(INFO, "Wait data: client_socket_fd %d", client_socket_fd);
            }
        }
    }
    return 0;
}

int main(void) {

    int server_socket_fd = create_nonblocking_server_socket();
    if (server_socket_fd < 0) {
        return server_socket_fd;
    }

    int epoll_fd = register_server_epoll_fd(server_socket_fd);

    start_server(server_socket_fd, 7181);

    epoll_loop(server_socket_fd, epoll_fd);

    LOG(INFO, "Hello, World!");
    return 0;
}
