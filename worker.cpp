#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

#include "common.h"

double compute_integral(double a, double b) {
    int n = 100000;
    double h = (b - a) / n;
    double s = 0.0;
    for (int i = 0; i < n; ++i) {
        s += function(a + i * h) * h;
    }
    sleep(2);
    return s;
}

int main() {
    int tcp_listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(tcp_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in listen_tcp_addr{};
    listen_tcp_addr.sin_family = AF_INET;
    listen_tcp_addr.sin_port = htons(WORKER_PORT);
    listen_tcp_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(tcp_listen_sock, (sockaddr *)&listen_tcp_addr, sizeof(listen_tcp_addr)) < 0) {
        std::cerr << "Failed to bind TCP socket" << std::endl;
        return 1;
    }

    if (listen(tcp_listen_sock, 5) < 0) {
        std::cerr << "Failed to listen on TCP socket" << std::endl;
        return 1;
    }

    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in listen_addr{};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(BROADCAST_PORT);
    listen_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(udp_sock, (sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        std::cerr << "Failed to bind socket" << std::endl;
        return 1;
    }

    char buffer[BUFFER_SIZE];
    sockaddr_in master_addr{};
    socklen_t addr_len = sizeof(master_addr);

    std::cout << "Waiting for master" << std::endl;
    recvfrom(udp_sock, buffer, BUFFER_SIZE, 0, (sockaddr *)&master_addr, &addr_len);
    std::cout << "Received message from master" << std::endl;

    sendto(udp_sock, "BRUH MOMENT", 3, 0, (sockaddr *)&master_addr, addr_len);
    close(udp_sock);

    while (true) {
        int tcp_sock = accept(tcp_listen_sock, nullptr, nullptr);
        if (tcp_sock < 0) {
            std::cerr << "Failed to accept connection" << std::endl;
            return 1;
        }
        std::cout << "Accepted connection" << std::endl;

        while (true) {
            int segment_id;
            ssize_t n = recv(tcp_sock, &segment_id, sizeof(segment_id), 0);
            if (n == sizeof(segment_id)) {
                std::cout << "Received task for segment " << segment_id << std::endl;
                if (segment_id >= 0 && segment_id < N_SEGMENTS) {
                    Segment seg = Segment(SEGMENT_START, SEGMENT_END, N_SEGMENTS, segment_id);
                    double result_value = compute_integral(seg.start, seg.end);

                    struct {
                        int segment_id;
                        double value;
                    } res;
                    res.segment_id = segment_id;
                    res.value = result_value;

                    send(tcp_sock, &res, sizeof(res), 0);
                } else {
                    throw std::runtime_error("Invalid segment ID");
                }
            } else if (n <= 0) {
                std::cout << n << ' ' << errno << std::endl;
                usleep(100'000);
                break;
            } else {
                continue;
            }
        }

        close(tcp_sock);
        std::cout << "Connection closed" << std::endl;
    }

    close(tcp_listen_sock);
    return 0;
}
