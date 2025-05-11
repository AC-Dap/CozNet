#ifndef PROFILER_H
#define PROFILER_H

#include <linux/perf_event.h>
#include <time.h>
#include <unistd.h>
#include <cstdint>
#include <signal.h>

struct Profiler {
    Profiler(): ring_buffer(nullptr), perf_fd(-1), timer_delay_ns(0), processing(false),
        hit_counts(0), profile_counts(0) {}

    // Initializes the profiler, but does not start it.
    bool init(uint64_t profiled_ip, size_t sample_period, size_t batch_size, size_t timer_period);
    bool start();
    bool stop();

    inline size_t get_hit_counts() { return hit_counts; }
    inline size_t get_profile_counts() { return profile_counts; }

    void process_samples();

private:
    // Copies from ring_buffer. Assumes the data is actually available.
    void copy_from_ring_buffer(size_t index, void* buf, size_t len);

    // Per the man page, the ring buffers should be (1 + 2^n) pages long.
    static constexpr size_t RING_BUFFER_DATA_PAGES = 1<<3;
    static constexpr size_t RING_BUFFER_HEADER_SIZE = 0x1000;
    static constexpr size_t RING_BUFFER_DATA_SIZE = RING_BUFFER_DATA_PAGES * 0x1000;
    static constexpr size_t RING_BUFFER_SIZE = RING_BUFFER_DATA_SIZE + RING_BUFFER_HEADER_SIZE;
    struct perf_event_mmap_page* ring_buffer;
    int perf_fd;

    timer_t timer;
    size_t timer_delay_ns;
    bool processing;

    uint64_t profiled_ip;
    size_t hit_counts;
    size_t profile_counts;
};

void sigaction_process_samples(int signum, siginfo_t* info, void* ctx);

#endif //PROFILER_H
