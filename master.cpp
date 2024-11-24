#include <iostream>
#include <netinet/in.h>
#include <unordered_map>
#include <vector>
#include <unordered_set>
#include <queue>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <poll.h>
#include <netinet/tcp.h>

#ifndef SOCK_NONBLOCK
#include <fcntl.h>
#define SOCK_NONBLOCK O_NONBLOCK
#endif

#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE TCP_KEEPALIVE
#endif

#include "common.h"

#define MAX_FDS 1024
#define RECONNECT_INTERVAL 5
#define POLL_TIMEOUT 1000

class Answer {
    double value;
    std::unordered_set<int> completed_segments;

public:
    Answer() : value(0.0) {}

    void add_segment(int segment_id, double result) {
        if (completed_segments.find(segment_id) == completed_segments.end()) {
            value += result;
            completed_segments.insert(segment_id);
        }
    }

    bool is_complete() {
        return completed_segments.size() == N_SEGMENTS;
    }

    double get_value() {
        return value;
    }
};

enum class WorkerState {
    Disconnected,
    Connecting,
    Connected,
    Busy
};

class Worker {
    int id;
    int sockfd;
    sockaddr_in addr;
    WorkerState state;
    int segment_id;
    time_t last_attempt;

public:
    Worker(int id, sockaddr_in address)
        : id(id), sockfd(-1), addr(address), state(WorkerState::Disconnected), segment_id(-1), last_attempt(0) {}

    ~Worker() {
        close_socket();
    }

    void update(std::queue<int>& segment_queue) {
        switch (state) {
            case WorkerState::Disconnected:
                std::cout << "Attempting connection to worker " << id << std::endl;
                attempt_connect();
                break;
            case WorkerState::Connecting:
                std::cout << "Checking connection to worker " << id << std::endl;
                check_connection();
                break;
            case WorkerState::Connected:
                if (segment_id == -1 && !segment_queue.empty()) {
                    segment_id = segment_queue.front();
                    std::cout << "Sending task " << segment_id << " to worker " << id << std::endl;
                    if (send_task(segment_id)) {
                        segment_queue.pop();
                        state = WorkerState::Busy;
                    } else {
                        segment_id = -1;
                    }
                }
                break;
            case WorkerState::Busy:
                break;
        }
    }

    void handle_event(short revents, std::queue<int>& segment_queue, Answer& answer) {
        if (revents & POLLIN) {
            int seg_id;
            double result;
            bool disconnect = false;
            if (receive_result(seg_id, result, disconnect)) {
                if (seg_id == segment_id) {
                    answer.add_segment(seg_id, result);
                }
                segment_id = -1;
                state = WorkerState::Connected;
            } else if (disconnect) {
                handle_disconnect(segment_queue);
            }
        } else if (revents & (POLLHUP | POLLERR | POLLNVAL)) {
            handle_disconnect(segment_queue);
        }
    }

    int get_sockfd() {
        return sockfd;
    }

    short get_events() {
        if (state == WorkerState::Connecting) {
            return POLLOUT;
        } else if (state == WorkerState::Busy || state == WorkerState::Connected) {
            return POLLIN;
        }
        return 0;
    }

private:
    void attempt_connect() {
        time_t now = time(nullptr);
        if (now - last_attempt >= RECONNECT_INTERVAL) {
            if (initialize_socket()) {
                state = WorkerState::Connecting;
            }
            last_attempt = now;
        }
    }

    void check_connection() {
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
            close_socket();
            state = WorkerState::Disconnected;
        } else {
            state = WorkerState::Connected;
        }
    }

    bool initialize_socket() {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            return false;
        }

        int keepalive = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) < 0) {
            std::cerr << "Failed to set SO_KEEPALIVE, error: " << strerror(errno) << std::endl;
            close_socket();
            return false;
        }

        int keepidle = 10;
        if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle)) < 0) {
            std::cerr << "Failed to set TCP_KEEPIDLE, error: " << strerror(errno) << std::endl;
            close_socket();
            return false;
        }

        int keepintvl = 5;
        if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl)) < 0) {
            std::cerr << "Failed to set TCP_KEEPINTVL, error: " << strerror(errno) << std::endl;
            close_socket();
            return false;
        }

        int keepcnt = 3;
        if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt)) < 0) {
            std::cerr << "Failed to set TCP_KEEPCNT, error: " << strerror(errno) << std::endl;
            close_socket();
            return false;
        }

        set_nonblocking(sockfd);

        addr.sin_port = htons(WORKER_PORT);
        if (connect(sockfd, (sockaddr *)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS) {
            close_socket();
            return false;
        }

        return true;
    }

    bool send_task(int segment_id) {
        ssize_t sent = send(sockfd, &segment_id, sizeof(segment_id), 0);
        return sent == sizeof(segment_id);
    }

    bool receive_result(int &segment_id, double &result, bool &disconnect) {
        struct {
            int segment_id;
            double value;
        } res;
        ssize_t received = recv(sockfd, &res, sizeof(res), 0);
        if (received == sizeof(res)) {
            segment_id = res.segment_id;
            result = res.value;
            return true;
        } else if (received <= 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            disconnect = true;
        }
        return false;
    }

    void handle_disconnect(std::queue<int>& segment_queue) {
        if (segment_id != -1) {
            segment_queue.push(segment_id);
            segment_id = -1;
        }
        close_socket();
        state = WorkerState::Disconnected;
    }

    void close_socket() {
        if (sockfd != -1) {
            close(sockfd);
            sockfd = -1;
        }
    }
};

int main() {
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int opt = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    sockaddr_in broadcast_addr{};
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(BROADCAST_PORT);
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    const char *message = "HELLO THERE";
    for (int i = 0; i < BROADCAST_ATTEMPTS; i++) {
        sendto(udp_sock, message, strlen(message), 0, (sockaddr *)&broadcast_addr, sizeof(broadcast_addr));
        usleep(BROADCAST_INTERVAL_MILLIS);
    }

    std::vector<Worker> workers;
    pollfd udp_fds[1];
    udp_fds[0].fd = udp_sock;
    udp_fds[0].events = POLLIN;

    int discovery_timeout = 3000;
    int ret = poll(udp_fds, 1, discovery_timeout);
    while (ret > 0) {
        if (udp_fds[0].revents & POLLIN) {
            char buffer[BUFFER_SIZE];
            sockaddr_in worker_addr{};
            socklen_t addr_len = sizeof(worker_addr);
            recvfrom(udp_sock, buffer, BUFFER_SIZE, 0, (sockaddr *)&worker_addr, &addr_len);
            std::cout << "Received message from worker " << workers.size() << std::endl;

            Worker worker(workers.size(), worker_addr);
            workers.push_back(std::move(worker));
        }
        ret = poll(udp_fds, 1, discovery_timeout);
    }
    close(udp_sock);

    if (workers.empty()) {
        std::cerr << "No workers found" << std::endl;
        return 1;
    }
    std::cout << "Found " << workers.size() << " workers" << std::endl;

    std::queue<int> segment_queue;
    for (int i = 0; i < N_SEGMENTS; ++i) {
        segment_queue.push(i);
    }

    Answer answer;
    while (!answer.is_complete()) {
        std::vector<pollfd> fds;
        for (auto& worker : workers) {
            worker.update(segment_queue);
            int fd = worker.get_sockfd();
            if (fd != -1) {
                pollfd pfd;
                pfd.fd = fd;
                pfd.events = worker.get_events();
                pfd.revents = 0;
                fds.push_back(pfd);
            }
        }

        if (fds.empty()) {
            sleep(1);
            continue;
        }

        int ret = poll(fds.data(), fds.size(), POLL_TIMEOUT);
        if (ret > 0) {
            std::unordered_map<int, short> events;
            for (auto& pfd : fds) {
                events[pfd.fd] = pfd.revents;
            }
            for (auto& worker : workers) {
                int fd = worker.get_sockfd();
                auto it = events.find(fd);
                if (it != events.end()) {
                    worker.handle_event(it->second, segment_queue, answer);
                }
            }
        }
    }

    std::cout << "Total integral: " << answer.get_value() << std::endl;

    return 0;
}
