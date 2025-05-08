#include <linux/perf_event.h>    /* Definition of PERF_* constants */
#include <linux/hw_breakpoint.h> /* Definition of HW_* constants */
#include <sys/syscall.h>         /* Definition of SYS_* constants */
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <cerrno>
#include <iostream>
#include <cstring>
#include <cstdint>
#include <dlfcn.h>

#include "profiler.hpp"

extern Profiler p;
void sigaction_process_samples(int signum, siginfo_t* info, void* ctx) {
    p.process_samples();
}

// Largely copied from Coz
bool Profiler::init(uint64_t profiled_ip, size_t sample_period, size_t batch_size) {
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(struct perf_event_attr));
    pe.size = sizeof(struct perf_event_attr);

    //Profiler config
    pe.type = PERF_TYPE_SOFTWARE;
    pe.config = PERF_COUNT_SW_TASK_CLOCK;
    pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_CALLCHAIN;
    pe.sample_period = sample_period;
    pe.wakeup_events = batch_size; // This is ignored on linux 3.13 (why?)
    pe.exclude_idle = 1;
    pe.exclude_kernel = 1;
    pe.disabled = 1;

    // Init profiler
    perf_fd = syscall(SYS_perf_event_open, &pe, 0, -1, -1, PERF_FLAG_FD_CLOEXEC);
    if (perf_fd == -1) {
        std::cerr << "Failed to open perf_event: " << strerror(errno) << std::endl;
        return false;
    }

    // MMap ring buffer
    void* rb = mmap(NULL, RING_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, perf_fd, 0);
    if (rb == MAP_FAILED) {
        std::cerr << "Mapping perf_event ring buffer failed." << std::endl;
        return false;
    }
    ring_buffer = reinterpret_cast<struct perf_event_mmap_page*>(rb);

    // Set up timer
    struct sigevent ev;
    memset(&ev, 0, sizeof(ev));
    ev.sigev_signo = SIGPROF;
    ev.sigev_notify = SIGEV_THREAD_ID;
    ev._sigev_un._tid = gettid();

    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &ev, &timer) != 0) {
        std::cerr << "Failed to create timer!" << std::endl;
        return false;
    }
    timer_delay_ns = sample_period * batch_size;

    // Set up sigaction
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigaction_process_samples;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGPROF, &sa, nullptr);

    this->profiled_ip = profiled_ip;

    return true;
}

bool Profiler::start() {
    if(perf_fd == -1) {
        std::cerr << "Profiler is not initialized yet." << std::endl;
        return false;
    }

    // Start timer
    long ns = timer_delay_ns % 1000000000;
    time_t s = (timer_delay_ns - ns) / 1000000000;

    struct itimerspec ts;
    memset(&ts, 0, sizeof(ts));
    ts.it_interval.tv_sec = s;
    ts.it_interval.tv_nsec = ns;
    ts.it_value.tv_sec = s;
    ts.it_value.tv_nsec = ns;

    if (timer_settime(timer, 0, &ts, nullptr) != 0) {
        std::cerr << "Failed to start interval timer" << std::endl;
        return false;
    }

    // Start perf
    if (ioctl(perf_fd, PERF_EVENT_IOC_ENABLE, 0) == -1) {
        std::cerr << "Failed to start perf event: " << strerror(errno) << std::endl;
        timer_delete(timer);
        return false;
    }
    return true;
}

bool Profiler::stop() {
    if(perf_fd == -1) {
        std::cerr << "Profiler is not initialized yet." << std::endl;
        return false;
    }
    if (timer_delete(timer) != 0) {
        std::cerr << "Failed to stop timer" << std::endl;
        return false;
    }
    if (ioctl(perf_fd, PERF_EVENT_IOC_DISABLE, 0) == -1) {
        std::cerr << "Failed to stop perf event: " << strerror(errno) << std::endl;
        return false;
    }
    close(perf_fd);
    munmap(ring_buffer, RING_BUFFER_SIZE);
    return true;
}

// Copies from ring_buffer. Assumes the data is actually available.
void Profiler::copy_from_ring_buffer(size_t index, void* buf, size_t len) {
    uintptr_t base = reinterpret_cast<uintptr_t>(ring_buffer) + RING_BUFFER_HEADER_SIZE;
    size_t start_index = index % RING_BUFFER_DATA_SIZE;
    size_t end_index = start_index + len;

    if(end_index <= RING_BUFFER_DATA_SIZE) {
        memcpy(buf, reinterpret_cast<void*>(base + start_index), len);
    } else {
        size_t chunk2_size = end_index - RING_BUFFER_DATA_SIZE;
        size_t chunk1_size = len - chunk2_size;

        void* chunk2_dest = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(buf) + chunk1_size);

        memcpy(buf, reinterpret_cast<void*>(base + start_index), chunk1_size);
        memcpy(chunk2_dest, reinterpret_cast<void*>(base), chunk2_size);
    }
}

void Profiler::process_samples() {
    if (processing) return;
    processing = true;

    // Read ring_buffer head for index and tail
    if (!ring_buffer) {
        std::cerr << "Trying to process samples before ring buffer is initialized!" << std::endl;
        return;
    }
    struct perf_event_mmap_page *ring_buf_info = reinterpret_cast<struct perf_event_mmap_page*>(ring_buffer);
    size_t head = ring_buf_info->data_head;
    size_t tail = ring_buf_info->data_tail;

    // Loop from index to head
    struct perf_event_header hdr;
    char record[4096];
    while(tail + sizeof(hdr) < head) {
        // Copy in packet
        copy_from_ring_buffer(tail, &hdr, sizeof(hdr));
        copy_from_ring_buffer(tail + sizeof(hdr), record, hdr.size);
        tail += hdr.size;

        // Process ip and callstack
        uint64_t ip;
        memcpy(&ip, record, sizeof(uint64_t));
        if (ip == profiled_ip) hit_counts++;

        // Process callstack
        uint64_t nr;
        memcpy(&nr, record + sizeof(uint64_t), sizeof(uint64_t));
        for (size_t i = 0; i < nr; i++) {
            memcpy(&ip, record + (i+2) * sizeof(uint64_t), sizeof(uint64_t));
            if (ip == profiled_ip) hit_counts++;
        }

        profile_counts++;
    }

    processing = false;
}
