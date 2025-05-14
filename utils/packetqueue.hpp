#ifndef PACKETQUEUE_HPP
#define PACKETQUEUE_HPP

#include <ctime>
#include <stdexcept>

#include "mempool.hpp"

struct Packet {
    MemoryPoolBuffer *buffer;
    size_t len;
    size_t nread;
    timespec wakeup_time;
};

struct PacketQueue {

    PacketQueue(): head(0), tail(0), size(0) {};

    size_t get_size() { return size; }

    Packet* get_head() { return &ring_buffer[head]; }

    void push(Packet packet) {
        if (size == BUFFER_SIZE) throw std::runtime_error("Packet queue is full");

        ring_buffer[tail] = packet;
        tail = (tail + 1) % BUFFER_SIZE;
        size++;
    }

    Packet pop() {
        if (size == 0) throw std::runtime_error("Packet queue is empty");

        Packet ret = ring_buffer[head];
        head = (head + 1) % BUFFER_SIZE;
        size--;
        return ret;
    }

private:
    static constexpr size_t BUFFER_SIZE = 1024;
    Packet ring_buffer[BUFFER_SIZE];

    size_t head;
    size_t tail;
    size_t size;
};

#endif //PACKETQUEUE_HPP
