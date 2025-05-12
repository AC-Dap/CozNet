#ifndef HOOK_H
#define HOOK_H

#include "utils/packetqueue.hpp"

struct FD {
    bool is_socket;
    PacketQueue* pq;

    ~FD() {
        if (pq) delete pq;
    }
};

struct PacketMetadata {
    size_t number_server_calls;
};

#endif //HOOK_H
