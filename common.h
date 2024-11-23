#include <cmath>
#include <fcntl.h>

#define BROADCAST_PORT 5000
#define WORKER_PORT 6000
#define BUFFER_SIZE 1024
#define N_SEGMENTS 100
#define SEGMENT_START 0.0
#define SEGMENT_END M_PI

double function(double x) {
    return sin(x);
}

inline int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct Segment {
    double start;
    double end;

    Segment(double l, double r, int total_segments, int segment_id) {
        start = l + segment_id * (r - l) / total_segments;
        end = l + (segment_id + 1) * (r - l) / total_segments;
    }
};
