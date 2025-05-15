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
#include <unordered_map>

#include "hook.hpp"
#include "profiler.hpp"
#include "utils/mempool.hpp"
#include "utils/time.hpp"

typedef int(*execve_t)(const char *pathname, char *const argv[], char *const envp[]);
typedef int (*main_fn_t)(int, char**, char**);

execve_t real_execve = nullptr;
main_fn_t real_main = nullptr;

// Global data structures
Profiler p;

// How much a speedup call should virtually delay for
size_t delay_length_ns = 0;

// How much we've virtually delayed
uint64_t delayed_ns = 0;

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
	char dcuz_speedup_envp[256];

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
	if (reconstruct_envp("DCUZ_SPEEDUP", dcuz_speedup_envp, 256)) {
		new_envp[ncopied] = dcuz_speedup_envp;
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

	char* dcuz_speedup = getenv("DCUZ_SPEEDUP");
	if (!dcuz_speedup) {
		std::cerr << "DCUZ_SPEEDUP not found, running without speedup." << std::endl;
	} else {
		delay_length_ns = std::stof(dcuz_speedup) * 10000;
	}

	if (!found) {
		std::cerr << "Unable to find correct module and offset (" << module_name << ":" << module_offset << "), running without profiler." << std::endl;
		return real_main(argc, argv, env);
	}

	if (!p.init(ip, 10000, 10, 1e6)) {
		std::cerr << "Failed to initialize profiler, running without it." << std::endl;
		return real_main(argc, argv, env);
	}

	if (!p.start()) {
		std::cerr << "Failed to start profiler, running without it." << std::endl;
		return real_main(argc, argv, env);
	}

	// Run the real main function
	timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	int result = real_main(argc, argv, env);
	timespec end;
	clock_gettime(CLOCK_MONOTONIC, &end);

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
		long billion = 1000000000L;
		long ns_passed = billion * (end.tv_sec - start.tv_sec) + (long)(end.tv_nsec) - (long)(start.tv_nsec);
		delayed_ns += p.get_hit_counts() * delay_length_ns;

		outf << module_name << std::endl;
		outf << module_offset << std::endl;
		outf << dcuz_speedup << std::endl;
		outf << p.get_hit_counts() << std::endl;
		outf << p.get_profile_counts() << std::endl;
		outf << delayed_ns << std::endl;
		outf << ns_passed << std::endl;
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
