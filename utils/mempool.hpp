#ifndef MEMPOOL_H
#define MEMPOOL_H

#include <list>

struct MemoryPoolBuffer {
    char* buffer;

    MemoryPoolBuffer* next;

    MemoryPoolBuffer(size_t len): next(nullptr) {
        buffer = new char[len];
    }

    ~MemoryPoolBuffer() {
        delete[] buffer;
    }
};

struct MemoryPool {
    MemoryPool(size_t size, size_t buf_len): head(nullptr) {
        for (int i = 0; i < size; i++) {
            return_buf(new MemoryPoolBuffer(buf_len));
        }
    }

    void return_buf(MemoryPoolBuffer* buf) {
        if (!head) {
            return;
        }
        buf->next = head;
        head = buf;
    }

    MemoryPoolBuffer* get_buf() {
        MemoryPoolBuffer* buf = head;
        if (buf) {
            head = buf->next;
        }
        return buf;
    }

    // Note: This only cleans up buffers currently in the pool.
    // If buffers are checked out and not returned, they are leaked
    // unless the user manually deletes them.
    ~MemoryPool() {
        MemoryPoolBuffer* current = head;
        while (current) {
            MemoryPoolBuffer* temp = current;
            current = current->next;
            delete temp;
        }
    }

private:
    MemoryPoolBuffer* head;
};

#endif //MEMPOOL_H
