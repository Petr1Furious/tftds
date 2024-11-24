#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <poll.h>

#include "common.h"

double compute_integral(double a, double b) {
    int n = 100000;
    double h = (b - a) / n;
    double s = 0.0;
    for (int i = 0; i < n; ++i) {
        s += function(a + i * h) * h;
    }
    usleep(1'000'000);
    return s;
}

int main() {
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        std::cerr << "Failed to create UDP socket" << std::endl;
        return 1;
    }

    sockaddr_in listen_udp_addr{};
    listen_udp_addr.sin_family = AF_INET;
    listen_udp_addr.sin_port = htons(BROADCAST_PORT);
    listen_udp_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(udp_sock, (sockaddr *)&listen_udp_addr, sizeof(listen_udp_addr)) < 0) {
        std::cerr << "Failed to bind UDP socket" << std::endl;
        close(udp_sock);
        return 1;
    }

    int tcp_listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_listen_sock < 0) {
        std::cerr << "Failed to create TCP socket" << std::endl;
        close(udp_sock);
        return 1;
    }

    int opt = 1;
    setsockopt(tcp_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in listen_tcp_addr{};
    listen_tcp_addr.sin_family = AF_INET;
    listen_tcp_addr.sin_port = htons(WORKER_PORT);
    listen_tcp_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(tcp_listen_sock, (sockaddr *)&listen_tcp_addr, sizeof(listen_tcp_addr)) < 0) {
        std::cerr << "Failed to bind TCP socket" << std::endl;
        close(udp_sock);
        close(tcp_listen_sock);
        return 1;
    }

    if (listen(tcp_listen_sock, 5) < 0) {
        std::cerr << "Failed to listen on TCP socket" << std::endl;
        close(udp_sock);
        close(tcp_listen_sock);
        return 1;
    }

    struct pollfd fds[2];
    fds[0].fd = udp_sock;
    fds[0].events = POLLIN;
    fds[1].fd = tcp_listen_sock;
    fds[1].events = POLLIN;

    std::cout << "Worker is running and waiting for events..." << std::endl;

    while (true) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            std::cerr << "Poll error: " << strerror(errno) << std::endl;
            break;
        }

        if (fds[0].revents & POLLIN) {
            char buffer[BUFFER_SIZE];
            sockaddr_in master_addr{};
            socklen_t addr_len = sizeof(master_addr);
            ssize_t n = recvfrom(udp_sock, buffer, BUFFER_SIZE, 0, (sockaddr *)&master_addr, &addr_len);
            if (n > 0) {
                std::cout << "Received UDP message from master" << std::endl;
                sendto(udp_sock, "BRUH MOMENT", strlen("BRUH MOMENT"), 0, (sockaddr *)&master_addr, addr_len);
            }
        }

        if (fds[1].revents & POLLIN) {
            int tcp_sock = accept(tcp_listen_sock, nullptr, nullptr);
            if (tcp_sock < 0) {
                std::cerr << "Failed to accept TCP connection" << std::endl;
                continue;
            }

            std::cout << "Accepted TCP connection" << std::endl;

            while (true) {
                int segment_id;
                ssize_t n = recv(tcp_sock, &segment_id, sizeof(segment_id), 0);
                if (n == sizeof(segment_id)) {
                    std::cout << "Received task for segment " << segment_id << std::endl;
                    if (segment_id >= 0 && segment_id < N_SEGMENTS) {
                        Segment seg(SEGMENT_START, SEGMENT_END, N_SEGMENTS, segment_id);
                        double result_value = compute_integral(seg.start, seg.end);

                        struct {
                            int segment_id;
                            double value;
                        } res{segment_id, result_value};

                        send(tcp_sock, &res, sizeof(res), 0);
                    } else {
                        std::cerr << "Invalid segment ID" << std::endl;
                    }
                } else if (n == 0) {
                    std::cout << "TCP connection closed by master" << std::endl;
                    break;
                } else if (n < 0) {
                    std::cerr << "Error receiving data: " << strerror(errno) << std::endl;
                    break;
                }
            }

            close(tcp_sock);
            std::cout << "Closed TCP connection" << std::endl;
        }
    }

    close(udp_sock);
    close(tcp_listen_sock);
    return 0;
}
