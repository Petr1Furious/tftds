#include <iostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <poll.h>
#include <netinet/tcp.h>

#include "common.h"

#define MAX_FDS 1024

class Worker {
    int sockfd;
    sockaddr_in addr;
    bool available;

public:
    Worker(sockaddr_in address)
        : sockfd(-1), addr(address), available(true) {}

    ~Worker() {
        if (sockfd != -1) {
            close(sockfd);
        }
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    Worker(Worker&& other) noexcept
        : sockfd(other.sockfd), addr(other.addr), available(other.available) {
        other.sockfd = -1;
    }

    Worker& operator=(Worker&& other) noexcept {
        if (this != &other) {
            close_socket();
            sockfd = other.sockfd;
            addr = other.addr;
            available = other.available;
            other.sockfd = -1;
        }
        return *this;
    }

    bool initialize_socket() {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            std::cerr << "Failed to create socket, error: " << strerror(errno) << std::endl;
            return false;
        }

        int keepalive = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
        set_nonblocking(sockfd);

        addr.sin_port = htons(WORKER_PORT);
        if (connect(sockfd, (sockaddr *)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS) {
            std::cerr << "Failed to connect to worker, error: " << strerror(errno) << std::endl;
            close(sockfd);
            sockfd = -1;
            return false;
        }

        return true;
    }

    bool send_task(int segment_id) {
        ssize_t sent = send(sockfd, &segment_id, sizeof(segment_id), 0);
        if (sent == sizeof(segment_id)) {
            std::cout << "Sent task " << segment_id << " to worker " << sockfd << std::endl;
            available = false;
            return true;
        }
        std::cerr << "Failed to send task " << segment_id << " to worker " << sockfd << std::endl;
        return false;
    }

    bool receive_result(int &segment_id, double &result) {
        struct {
            int segment_id;
            double value;
        } res;
        ssize_t received = recv(sockfd, &res, sizeof(res), 0);
        if (received == sizeof(res)) {
            segment_id = res.segment_id;
            result = res.value;
            available = true;
            return true;
        }
        return false;
    }

    bool is_disconnected() {
        if (sockfd == -1) {
            return true;
        }
        char buf;
        ssize_t result = recv(sockfd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (result == 0) {
            return true;
        }
        if (result < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            }
            if (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN) {
                return true;
            }
            fprintf(stderr, "Unexpected error in is_disconnected: %s\n", strerror(errno));
        }
        return true;
    }

    bool is_available() {
        return available;
    }

    int get_sockfd() {
        return sockfd;
    }

    void close_socket() {
        close(sockfd);
        sockfd = -1;
    }
};

class Asnwer {
    double value;
    std::unordered_set<int> completed_segments;

public:
    Asnwer() : value(0.0) {}

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

void disconnect_client(Worker& worker, std::queue<int>& segment_queue, std::unordered_map<int, int>& worker_to_segment) {
    auto it = worker_to_segment.find(worker.get_sockfd());
    if (it != worker_to_segment.end()) {
        int segment_id = it->second;
        segment_queue.push(segment_id);
        worker_to_segment.erase(it);
    }
    worker.close_socket();
}

int main() {
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int opt = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    sockaddr_in broadcast_addr{};
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(BROADCAST_PORT);
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    const char *message = "HELLO THERE";
    sendto(udp_sock, message, strlen(message), 0, (sockaddr *)&broadcast_addr, sizeof(broadcast_addr));

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
            std::cout << "Received message from worker" << std::endl;

            Worker worker(worker_addr);
            if (worker.initialize_socket()) {
                workers.push_back(std::move(worker));
            }
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

    std::unordered_map<int, int> worker_to_segment; // sockfd -> segment_id

    size_t nfds = workers.size();
    pollfd fds[MAX_FDS];
    for (size_t i = 0; i < workers.size(); ++i) {
        fds[i].fd = workers[i].get_sockfd();
        fds[i].events = POLLIN | POLLHUP | POLLERR;
    }

    for (size_t i = 0; i < workers.size(); ++i) {
        if (!segment_queue.empty()) {
            int segment_id = segment_queue.front();
            if (workers[i].send_task(segment_id)) {
                segment_queue.pop();
                worker_to_segment[workers[i].get_sockfd()] = segment_id;
            } else {
                segment_queue.push(segment_id);
            }
        }
    }

    Asnwer answer;
    while (!answer.is_complete()) {
        ret = poll(fds, nfds, -1);
        if (ret > 0) {
            for (size_t i = 0; i < workers.size(); ++i) {
                if (fds[i].revents & POLLIN) {
                    int segment_id;
                    double result_value;
                    if (workers[i].receive_result(segment_id, result_value)) {
                        std::cout << "Received result for segment " << segment_id << " from worker " << workers[i].get_sockfd() << std::endl;
                        answer.add_segment(segment_id, result_value);
                        worker_to_segment.erase(workers[i].get_sockfd());
                    } else {
                        std::cerr << "Failed to receive result from worker " << workers[i].get_sockfd() << std::endl;
                    }
                } else if (fds[i].revents & (POLLHUP | POLLERR)) {
                    std::cerr << "Worker " << workers[i].get_sockfd() << " disconnected" << std::endl;
                    disconnect_client(workers[i], segment_queue, worker_to_segment);
                } else if (fds[i].revents != 0 && workers[i].get_sockfd() != -1) {
                    if (fds[i].revents & POLLNVAL) {
                        std::cerr << "Invalid file descriptor for worker " << workers[i].get_sockfd() << std::endl;
                        disconnect_client(workers[i], segment_queue, worker_to_segment);
                    } else {
                        std::cerr << "Unexpected event " << fds[i].revents << std::endl;
                        return 1;
                    }
                }

                if (workers[i].is_available() && !segment_queue.empty()) {
                    if (workers[i].is_disconnected()) {
                        std::cerr << "Worker " << workers[i].get_sockfd() << " disconnected" << std::endl;
                        if (workers[i].get_sockfd() != -1) {
                            workers[i].close_socket();
                        }
                        if (!workers[i].initialize_socket()) {
                            continue;
                        }
                    }

                    int segment_id = segment_queue.front();
                    if (workers[i].send_task(segment_id)) {
                        segment_queue.pop();
                        worker_to_segment[workers[i].get_sockfd()] = segment_id;
                    }
                }
            }
        }

        bool everyone_disconnected = true;
        for (size_t i = 0; i < workers.size(); ++i) {
            if (workers[i].get_sockfd() != -1) {
                everyone_disconnected = false;
                break;
            }
        }
        if (everyone_disconnected) {
            std::cerr << "All workers disconnected, sleeping for 1 second" << std::endl;
            sleep(1);
        }
    }

    std::cout << "Total integral: " << answer.get_value() << std::endl;

    return 0;
}
