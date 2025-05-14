#ifndef SOCKET_HOOK_HPP
#define SOCKET_HOOK_HPP

struct PacketMetadata {
    uint32_t number_server_calls;
    uint32_t total_virtual_delay;
    uint32_t data_size;
};

#endif //SOCKET_HOOK_HPP
