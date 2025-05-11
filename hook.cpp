#define _GNU_SOURCE

#include <cstring>
#include <unistd.h>
#include <dlfcn.h>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <sys/stat.h>
#include <ctime>
#include <poll.h>
#include <algorithm>
#include <iostream>
#include <link.h>
#include <fstream>

#include "hook.hpp"
#include "profiler.hpp"
#include "utils/mempool.hpp"
#include "utils/time.hpp"
#include "utils/waitqueue.hpp"

constexpr size_t MAGIC = 0xabcdeffedcba;
constexpr size_t PACKET_SIZE = 1024;

typedef int(*execve_t)(const char *pathname, char *const argv[], char *const envp[]);
typedef ssize_t(*read_t)(int fd, void *buf, size_t count);
typedef ssize_t(*write_t)(int fd, const void *buf, size_t count);
typedef int (*main_fn_t)(int, char**, char**);

execve_t real_execve = nullptr;
read_t real_read = nullptr;
write_t real_write = nullptr;
main_fn_t real_main = nullptr;

// Global data structures
WaitQueue wq;
MemoryPool mp(1024, PACKET_SIZE);
Profiler p;

bool reconstruct_envp(const char* env_name, char* envp, size_t envp_len) {
	const char* env = getenv(env_name);
	// env_name=env
	if (env == nullptr || strlen(env_name) + 1 + strlen(env) > envp_len - 1) {
		return false;
	}
	sprintf(envp, "%s=%s", env_name, env);
	return true;
}

/*
	We hook into execve to ensure this file is LD_PRELOADed across execs.
*/
extern "C" int execve(const char *pathname, char *const argv[], char *const envp[]) {
	if (!real_execve) {
		real_execve = (execve_t) dlsym(RTLD_NEXT, "execve");
	}

	char* new_envp[256];
	size_t ncopied = 0;
	char ld_preload_envp[256];
	char dcuz_module_envp[256];
	char dcuz_offset_envp[256];

	// Copy over necessary env vars
	if (reconstruct_envp("LD_PRELOAD", ld_preload_envp, 256)) {
		new_envp[ncopied] = ld_preload_envp;
		ncopied++;
	}
	if (reconstruct_envp("DCUZ_MODULE", dcuz_module_envp, 256)) {
		new_envp[ncopied] = dcuz_module_envp;
		ncopied++;
	}
	if (reconstruct_envp("DCUZ_OFFSET", dcuz_offset_envp, 256)) {
		new_envp[ncopied] = dcuz_offset_envp;
		ncopied++;
	}

	// Copy over other envp
	for (int i = ncopied; i < 100; i++) {
		new_envp[i] = envp[i - ncopied];

		if (new_envp[i] == nullptr) break;
		if (i == 99) return E2BIG; // At end but no nullptr seen, too many env vars
	}

	return real_execve(pathname, argv, new_envp);
}

/**
	Issue a blocking read to `fd`. Reads in a packet, then parses the metadata and adds it to the waitqueue
	with the associated delay time.

	Assumes packets will always be 256 bytes or less.
	Calls malloc for the packet data that is stored to the waitqueue.
*/
ssize_t read_to_waitqueue(int fd) {
	MemoryPoolBuffer *mp_buf = mp.get_buf();
	if (!mp_buf) {
		fprintf(stderr, "Memory Pool empty!\n");
		return -1;
	}
	char* temp_buf = mp_buf->buffer;
	ssize_t n = real_read(fd, temp_buf, PACKET_SIZE);
	if (n <= 0) {
		return n;
	}

	// Default if there is no metadata
	timespec wakeup_time;
	clock_gettime(CLOCK_MONOTONIC, &wakeup_time);
	size_t data_offset = 0;

	// Parse metadata
	// First check size + read magic number
	size_t temp_buf_magic = 0;
	if (n > sizeof(MAGIC) + sizeof(PacketMetadata)) memcpy(&temp_buf_magic, temp_buf, sizeof(MAGIC));

	if (temp_buf_magic == MAGIC) {
		PacketMetadata meta;
		memcpy(&meta, temp_buf + sizeof(MAGIC), sizeof(PacketMetadata));

		// Add required delay to wakeup time
		add_ns(&wakeup_time, 10000 * meta.number_server_calls);

		// Set data offset past metadata
		data_offset = sizeof(MAGIC) + sizeof(PacketMetadata);
	}

	// Add new packet to wait queue
	// Use nread as starting offset
	WaitQueueEntry entry = { .buffer = mp_buf, .len = size_t(n), .nread = data_offset, .wakeup_time = wakeup_time };
	wq.push_entry(entry);

	return n;
}

extern "C" ssize_t read(int fd, void *buf, size_t count) {
    if (!real_read) {
        real_read = (read_t) dlsym(RTLD_NEXT, "read");
    }

    // Passthrough for non-socket fds
    struct stat statbuf;
    if (fstat(fd, &statbuf) == -1 || !S_ISSOCK(statbuf.st_mode)) {
        return real_read(fd, buf, count);
    }

	// If wait queue is currently empty, do a blocking read for a new packet
	if (wq.get_size() == 0) {
		ssize_t ret = read_to_waitqueue(fd);
		if (ret <= 0) return ret;
	}
	// Now, we're guaranteed wait queue has at least one element

	// Now process packet in wait queue
	timespec now;
    while (true) {
        WaitQueueEntry *head = wq.get_head();
    	clock_gettime(CLOCK_MONOTONIC, &now);

        // Deliver if ready
        if (time_passed(head->wakeup_time, now)) {
            size_t avail = head->len - head->nread;
            size_t to_copy = std::min(avail, count);
            memcpy(buf, head->buffer->buffer + head->nread, to_copy);
            head->nread += to_copy;
            if (head->nread == head->len) {
				mp.return_buf(head->buffer);
                wq.pop_head();
            }
            return to_copy;
        }

        // Otherwise wait for either packet arrival or timeout
        timespec timeout = time_diff(head->wakeup_time, now);
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ready = ppoll(&pfd, 1, &timeout, nullptr);

		// New packet arrival
        if (ready > 0 && (pfd.revents & POLLIN)) {
			// Ignore any errors since we'll just return packets in our queue
        	read_to_waitqueue(fd);
        }
        // Timeout: packet head is now ready, loop again to process
    }
}

extern "C" ssize_t write(int fd, const void *buf, size_t count) {
	if (!real_write) {
		real_write = (write_t) dlsym(RTLD_NEXT, "write");
	}

	// If it's not socket, don't do anything special
	struct stat statbuf;
    if (fstat(fd, &statbuf) == -1 || !S_ISSOCK(statbuf.st_mode)) {
		return real_write(fd, buf, count);
	}

	char new_buf[PACKET_SIZE];

	// Copy over metadata before buf
	PacketMetadata meta = {0};
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

struct DlIterateData {
	const char* target_lib_name;
	uintptr_t base_address;
	bool found;
};

static int find_library_callback(struct dl_phdr_info *info, size_t size, void *data) {
	DlIterateData* search_data = static_cast<DlIterateData*>(data);

	char exe[1024];
	const char* dl_name = info->dlpi_name;
	if (!dl_name || dl_name[0] == '\0') {
		int ret = readlink("/proc/self/exe", exe, sizeof(exe)-1);
		if(ret == -1) {
			std::cerr << "Couldn't read /proc/self/exe" << std::endl;
			return 0;
		}
		exe[ret] = 0;
		dl_name = exe;
	}

	// std::cerr << "Loaded: " << dl_name << std::endl;

	if (dl_name && strstr(dl_name, search_data->target_lib_name)) {
		search_data->base_address = info->dlpi_addr; // This is the base address
		search_data->found = true;
		return 1; // Stop iteration
	}
	return 0; // Continue iteration
}

static int wrapped_main(int argc, char** argv, char** env) {
	// Read loaded modules
	bool found = false;
	uint64_t ip = 0;
	char* module_name = getenv("DCUZ_MODULE");
	char* module_offset = getenv("DCUZ_OFFSET");
	if (module_name && module_offset) {
		ip = std::stoi(module_offset, 0, 16);
		DlIterateData search_data = { .target_lib_name = module_name };
		dl_iterate_phdr(find_library_callback, &search_data);

		found = search_data.found;
		ip += search_data.base_address;
	} else {
		std::cerr << "DCUZ_MODULE or DCUZ_OFFSET not found, running without profiler." << std::endl;
		return real_main(argc, argv, env);
	}

	if (!found) {
		std::cerr << "Unable to find correct module and offset, running without profiler." << std::endl;
		return real_main(argc, argv, env);
	}

	// std::cerr << "Found module " << module_name << ", profiling ip 0x" << std::hex << ip << std::endl;
	if (!p.init(ip, 10000, 4, 1e7)) {
		std::cerr << "Failed to initialize profiler, running without it." << std::endl;
		return real_main(argc, argv, env);
	}

	if (!p.start()) {
		std::cerr << "Failed to start profiler, running without it." << std::endl;
		return real_main(argc, argv, env);
	}

	// Run the real main function
	int result = real_main(argc, argv, env);

	// Increment the end-to-end progress point just before shutdown
	/*if(end_to_end) {
		throughput_point* end_point =
		  profiler::get_instance().get_throughput_point("end-to-end");
		end_point->visit();
	}*/

	// Shut down the profiler
	p.stop();

	std::ofstream outf;
	std::string filename = std::to_string(getpid()) + ".txt";

	outf.open(filename);
	if (outf.is_open()) {
		outf << p.get_hit_counts() << std::endl;
		outf << p.get_profile_counts() << std::endl;
		outf.close();
	} else {
		std::cerr << p.get_hit_counts() << std::endl;
		std::cerr << p.get_profile_counts() << std::endl;
	}

	return result;
}

extern "C" int __libc_start_main(main_fn_t main_fn, int argc, char** argv,
	void (*init)(), void (*fini)(), void (*rtld_fini)(), void* stack_end) {
	// Save real main
	real_main = main_fn;

	auto real_libc_start_main = (decltype(__libc_start_main)*)dlsym(RTLD_NEXT, "__libc_start_main");

	// Call start_main with our main function
	return real_libc_start_main(wrapped_main, argc, argv, init, fini, rtld_fini, stack_end);
}
