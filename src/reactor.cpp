#include <memory.h>
#include <evchannel.h>
#include <log.h>
#include <timer.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <sys/fcntl.h>

constexpr static uint16_t listen_port = 80;

int setnoblocking(int fd) {
    int old = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, old | O_NONBLOCK);
    return old;
}

int sockfd;
bool flag = false;
int64_t ticks = 0;

void on_abort() {
    close(sockfd);
}

int main() {
    event_channel evchannel;
    log_init(evchannel);
    unlink("/tmp/tinyhttp_reactor_unsock");
    //创建Unix域套接字供事件总线使用
    int unsockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    constexpr std::string_view unsockaddrpath = "/tmp/tinyhttp_reactor_unsock";
    sockaddr_un unsockaddr {};
    unsockaddr.sun_family = AF_UNIX;
    memcpy(unsockaddr.sun_path, unsockaddrpath.data(), unsockaddrpath.length());
    if (bind(unsockfd, reinterpret_cast<sockaddr*>(&unsockaddr), sizeof(sockaddr_un)) == -1) {
        FATAL(std::format("error when bind unix domain socket: {}", strerror(errno)));
        exit(-1);
    }
    if (listen(unsockfd, SOMAXCONN) == -1) {
        FATAL(std::format("error when listen unix domain socket: {}", strerror(errno)));
        exit(-1);
    }
    sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in soaddr {};
    soaddr.sin_family = AF_INET;
    inet_aton("127.0.0.1", &soaddr.sin_addr);
    soaddr.sin_port = htons(listen_port);
    if (bind(sockfd, reinterpret_cast<sockaddr*>(&soaddr), sizeof(soaddr)) == -1) {
        FATAL(std::format("error when bind socket: {}", strerror(errno)));
        exit(-1);
    }
    if (listen(sockfd, SOMAXCONN) == -1) {
        FATAL(std::format("error when listen socket: {}", strerror(errno)));
        exit(-1);
    }
    INFO(std::format("server started at port {}", listen_port));
    int epfd = epoll_create(1024);
    epoll_event unsockev{};
    unsockev.events = EPOLLIN | EPOLLET;
    unsockev.data.fd = unsockfd;
    epoll_event sockev{};
    sockev.events = EPOLLIN | EPOLLET;
    sockev.data.fd = sockfd;
    setnoblocking(unsockfd);
    setnoblocking(sockfd);
    epoll_ctl(epfd, EPOLL_CTL_ADD, unsockfd, &unsockev);
    epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &sockev);
    epoll_event events[1024];
    std::thread console([&]() {
        std::string command;
        while (true) {
            std::cin >> command;
            if (command == "stop") {
                flag = true;
                std::this_thread::sleep_for(std::chrono::seconds(2));
                close(sockfd);
                break;
            }
        }
    });
    console.detach();
    std::vector<int> workers_fd;
    while (!flag) {
        int r = epoll_wait(epfd, events, 1024, 50);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            FATAL(std::format("error when epoll_wait(): {}", strerror(errno)));
            break;
        }
        //发送一次TickEvent
        for (auto& fd : workers_fd) {
            try {
                send_packet(fd, tick_event(ticks));
            } catch (io_exception& e) {
                e.print();
                auto c = std::remove_if(workers_fd.begin(), workers_fd.end(), [&](const int f) {
                    return f == fd;
                });
                workers_fd.erase(c);
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
            }
        }
        for (int i = 0; i < r; ++i) {
            int fd = events[i].data.fd;
            if (fd == sockfd) {
                if (events[i].events & EPOLLERR) {
                    FATAL(std::format("error from reactor socket: {}", strerror(errno)));
                    break;
                }
                sockaddr addr{};
                socklen_t len;
                int clifd = accept(sockfd, &addr, &len);
            } else if (fd == unsockfd) {

            }
        }
    }
    log_close();
}