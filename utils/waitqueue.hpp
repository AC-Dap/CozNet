#ifndef WAITQUEUE_H
#define WAITQUEUE_H

#include <vector>
#include <algorithm>
#include "mempool.hpp"

struct WaitQueueEntry {
    MemoryPoolBuffer *buffer;
    size_t len;
    size_t nread;
    timespec wakeup_time;

    bool operator<(const WaitQueueEntry& r) {
        return (this->wakeup_time.tv_sec < r.wakeup_time.tv_sec) ||
            (this->wakeup_time.tv_sec == r.wakeup_time.tv_sec && this->wakeup_time.tv_nsec < r.wakeup_time.tv_nsec);
    }
};

struct WaitQueue {
    // Initialize with capacity for 16 entries.
    WaitQueue() {
        entries.reserve(16);
    }

    inline WaitQueueEntry* get_head() {
        return entries.empty() ? nullptr : &entries[0];
    }

    inline size_t get_size() {
        return entries.size();
    }

    inline void pop_head() {
        std::pop_heap(entries.begin(), entries.end());
        entries.pop_back();
    }

    inline void push_entry(WaitQueueEntry entry) {
        entries.push_back(entry);
        std::push_heap(entries.begin(), entries.end());
    }

private:
    std::vector<WaitQueueEntry> entries;
};

#endif //WAITQUEUE_H
