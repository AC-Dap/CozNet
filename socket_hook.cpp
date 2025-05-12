#define _GNU_SOURCE

#include <cstring>
#include <unistd.h>
#include <dlfcn.h>
#include <ctime>
#include <poll.h>
#include <vector>
#include <utility>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <stdexcept>
#include <iostream>
#include <fcntl.h>

#include "utils/mempool.hpp"
#include "utils/time.hpp"
#include "utils/packetqueue.hpp"
#include "socket_hook.hpp"

constexpr size_t MAGIC = 0xabcdeffedcba;
constexpr size_t PACKET_SIZE = 1024;

typedef ssize_t(*read_t)(int fd, void *buf, size_t count);
typedef ssize_t(*write_t)(int fd, const void *buf, size_t count);
typedef int(*epoll_pwait_t)(int epfd, struct epoll_event events[], int maxevents, int timeout, const sigset_t* sigmask);
typedef int(*close_t)(int fd);
typedef int(*connect_t)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
typedef int(*accept_t)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
typedef int(*accept4_t)(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags);

read_t real_read = nullptr;
write_t real_write = nullptr;
epoll_pwait_t real_epoll_pwait = nullptr;
close_t real_close = nullptr;
connect_t real_connect = nullptr;
accept_t real_accept = nullptr;
accept4_t real_accept4 = nullptr;

std::vector<std::pair<int, PacketQueue*>> fds;
MemoryPool mp(1024, PACKET_SIZE);

PacketQueue* get_packet_queue(int fd) {
	for (int i = 0; i < fds.size(); i++) {
		if (fds[i].first == fd) return fds[i].second;
	}
	return nullptr;
}

bool initialized = false;
void initialize_real_functions() {
	if (initialized) return;

	real_read = (read_t)dlsym(RTLD_NEXT, "read");
	real_write = (write_t)dlsym(RTLD_NEXT, "write");
	real_epoll_pwait = (epoll_pwait_t)dlsym(RTLD_NEXT, "epoll_pwait");
	real_close = (close_t)dlsym(RTLD_NEXT, "close");
	real_connect = (connect_t)dlsym(RTLD_NEXT, "connect");
	real_accept = (accept_t)dlsym(RTLD_NEXT, "accept");
	real_accept4 = (accept4_t)dlsym(RTLD_NEXT, "accept4");

	if (!real_read || !real_write || !real_epoll_pwait || !real_close || !real_connect || !real_accept || !real_accept4) {
		// Critical error: failed to get real function pointers.
		// This usually means LD_PRELOAD is not set up correctly or the functions don't exist.
		std::cerr << "Socket_Hook: CRITICAL - Failed to dlsym real functions. Exiting." << std::endl;
		exit(1);
	}
	initialized = true;
}

/**
	Issue a blocking read to `fd`. Reads in a packet, then parses the metadata and adds it to
	the given packet queue with the associated delay time.
*/
ssize_t read_to_queue(int fd, PacketQueue* pq) {
	MemoryPoolBuffer *mp_buf = mp.get_buf();
	if (!mp_buf) {
		throw std::bad_alloc();
	}
	char* read_buf = mp_buf->buffer;
	ssize_t n = real_read(fd, read_buf, PACKET_SIZE);
	if (n <= 0) {
		mp.return_buf(mp_buf);
		return n;
	}

	// Default if there is no metadata
	timespec wakeup_time;
	clock_gettime(CLOCK_MONOTONIC, &wakeup_time);

	// We can have multiple packets in a read or broken up across multiple reads
	size_t nconsumed = 0;
	Packet entry { .buffer = mp.get_buf(), .len = 0, .nread = 0, .wakeup_time = wakeup_time };
	while(nconsumed < n) {
		if (entry.len == 0) {
			// Read MAGIC and metadata
			size_t packet_magic = 0;
			memcpy(&packet_magic, read_buf + nconsumed, sizeof(MAGIC));
			if (packet_magic != MAGIC) {
				// If no header, just treat rest of data as packet
				entry.len = n - nconsumed;
			} else {
				PacketMetadata meta;
				memcpy(&meta, read_buf + nconsumed + sizeof(MAGIC), sizeof(PacketMetadata));

				// Add required delay to wakeup time
				add_ns(&entry.wakeup_time, 10000 * meta.number_server_calls);

				// Set packet len
				entry.len = meta.data_size;
				nconsumed += sizeof(MAGIC) + sizeof(PacketMetadata);
			}
		}

		// Copy remaining bytes into entry
		size_t to_copy = std::min(entry.len - entry.nread, n - nconsumed);
		memcpy(entry.buffer->buffer, read_buf + nconsumed, to_copy);
		nconsumed += to_copy;
		entry.nread += to_copy;

		// Packet fully copied
		if (entry.len == entry.nread) {
			entry.nread = 0;
			pq->push(entry);
			// If there is another packet
			if (nconsumed < n) entry = Packet{ .buffer = mp.get_buf(), .len = 0, .nread = 0, .wakeup_time = wakeup_time };
		} else {
			// Nead to make another read
			n = real_read(fd, read_buf, PACKET_SIZE);
			if (n <= 0) {
				mp.return_buf(mp_buf);
				mp.return_buf(entry.buffer);
				return n;
			}
			nconsumed = 0;
		}
	}
	return n;
}

extern "C" ssize_t read(int fd, void *buf, size_t count) {
	initialize_real_functions();

    // Passthrough for non-socket fds
	PacketQueue* pq = get_packet_queue(fd);
    if (!pq) {
        return real_read(fd, buf, count);
    }

	// If wait queue is currently empty, do a blocking read for a new packet
	if (pq->get_size() == 0) {
		ssize_t ret = read_to_queue(fd, pq);
		if (ret <= 0) return ret;
	}
	// Now, we're guaranteed wait queue has at least one element

	// Now process packet in wait queue
	timespec now;
    while (true) {
        Packet* head = pq->get_head();
    	clock_gettime(CLOCK_MONOTONIC, &now);

        // Deliver if ready
        if (time_passed(head->wakeup_time, now)) {
            size_t avail = head->len - head->nread;
            size_t to_copy = std::min(avail, count);
            memcpy(buf, head->buffer->buffer + head->nread, to_copy);
            head->nread += to_copy;
            if (head->nread == head->len) {
				mp.return_buf(head->buffer);
                pq->pop();
            }
            return to_copy;
        }

        // Otherwise wait for timeout
        timespec timeout = time_diff(head->wakeup_time, now);
        int ready = ppoll(nullptr, 0, &timeout, nullptr);
        // Timeout: packet head is now ready, loop again to process
    }
}

extern "C" int epoll_pwait(int epfd, struct epoll_event events[], int maxevents, int timeout, const sigset_t* sigmask) {
	initialize_real_functions();
	int nfds = 0;

	// Track time spent, so we eventually timeout if we need to retry multiple times
	timespec start_time;
	int time_spent = 0;
	clock_gettime(CLOCK_MONOTONIC, &start_time);

	// First see if we have any queued fds that are ready
	for (int i = 0; i < fds.size(); i++) {
		PacketQueue* pq = fds[i].second;
		if (pq->get_size() > 0 && time_passed(pq->get_head()->wakeup_time, start_time)) {
			events[nfds].events = EPOLLIN;
			events[nfds].data.fd = fds[i].first;
			events[nfds].data.u32 = events[nfds].data.u64 = uint32_t(fds[i].first);
			nfds++;
		}
	}

	while(nfds == 0 && (timeout == -1 || (timeout != -1 && timeout > time_spent))) {
		nfds = real_epoll_pwait(epfd, events, maxevents, timeout - time_spent, sigmask);

		timespec end_time;
		clock_gettime(CLOCK_MONOTONIC, &end_time);

		// If epoll naturally times out or fails, return
		if (nfds <= 0) return nfds;

		// For each fd, check if we are actually read to return
		// Even if an fd has something in its queue, we still read to ensure that it doesn't trigger epoll again
		// When we want to exclude an fd from epoll, we swap curr and end and increment/decrement accordingly
		size_t curr = 0;
		size_t end = nfds - 1;
		for (int i = 0; i < nfds; i++) {
			if (events[curr].events & EPOLLIN) {
				int fd = events[curr].data.fd;
				PacketQueue* pq = get_packet_queue(fd);
				if (!pq) {
					curr++;
					continue;
				}

				read_to_queue(fd, pq);

				// Is head of queue ready?
				if (pq->get_size() > 0 && time_passed(pq->get_head()->wakeup_time, end_time)) {
					curr++;
				} else {
					std::swap(events[curr], events[end]);
					end--;
				}
			} else {
				curr++;
			}
		}

		nfds = end + 1;
		if (timeout != -1) {
			timespec diff = time_diff(end_time, start_time);
			time_spent = diff.tv_sec * 1000 + diff.tv_nsec / 1e6;
		}

		// Add fds that should also be awake but we previously read
		// NOTE: This is technically not correct since we don't know that all fds are tied to this event fd.
		for (int i = 0; i < fds.size(); i++) {
			PacketQueue* pq = fds[i].second;
			if (pq->get_size() > 0 && time_passed(pq->get_head()->wakeup_time, end_time)) {
				events[nfds].events = EPOLLIN;
				events[nfds].data.fd = fds[i].first;
				events[nfds].data.u32 = events[nfds].data.u64 = uint32_t(fds[i].first);
				nfds++;
			}
		}
	}

	return nfds;
}

extern "C" ssize_t write(int fd, const void *buf, size_t count) {
	initialize_real_functions();

	// Passthrough for non-socket fds
	PacketQueue* pq = get_packet_queue(fd);
	if (!pq) {
		return real_write(fd, buf, count);
	}

	char new_buf[PACKET_SIZE];

	// Copy over metadata before buf
	PacketMetadata meta = { .number_server_calls = 0, .data_size = uint32_t(count) };
	memcpy(new_buf, &MAGIC, sizeof(MAGIC));
	memcpy(new_buf + sizeof(MAGIC), &meta, sizeof(PacketMetadata));

	// Copy buf into remaining space
	size_t new_count = std::min(count + sizeof(MAGIC) + sizeof(PacketMetadata), PACKET_SIZE);
	memcpy(new_buf + sizeof(MAGIC) + sizeof(PacketMetadata), buf, new_count - sizeof(MAGIC) - sizeof(PacketMetadata));

	int ret = real_write(fd, new_buf, new_count);

	// Hide metadata written
	if (ret < 0) return ret;
	return ret - sizeof(MAGIC) - sizeof(PacketMetadata);
}

extern "C" int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
	initialize_real_functions();

	// Now that we know sockfd is a socket fd used for reading/writing, add it to our fds map
	fds.emplace_back(sockfd, new PacketQueue());
	return real_connect(sockfd, addr, addrlen);
}

extern "C" int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
	initialize_real_functions();

	int fd = real_accept(sockfd, addr, addrlen);
	if (fd > 0) {
		// Create entry in fds map for new socket fd
		fds.emplace_back(fd, new PacketQueue());
	}
	return fd;
}

extern "C" int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
	initialize_real_functions();

	int fd = real_accept4(sockfd, addr, addrlen, flags);
	if (fd > 0) {
		// Create entry in fds map for new socket fd
		fds.emplace_back(fd, new PacketQueue());
	}
	return fd;
}

extern "C" int close(int fd) {
	initialize_real_functions();

	// Remove entry from fds map
	for (auto it = fds.begin(); it != fds.end(); it++) {
		if (it->first == fd) {
			delete it->second;
			fds.erase(it);
			break;
		}
	}
	return real_close(fd);
}