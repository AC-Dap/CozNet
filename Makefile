PKG_CFLAGS=$(shell pkg-config --cflags --libs raft libuv)
PKG_RPATH=$(shell pkg-config --variable=libdir raft)

CPP_FILES=hook.cpp profiler.cpp socket_hook.cpp
HPP_FILES=hook.hpp profiler.hpp socket_hook.hpp utils/mempool.hpp utils/time.hpp

all: cluster server dcuz

cluster: cluster.c
	cc cluster.c -o cluster $(PKG_CFLAGS) -g -Wl,-rpath=$(PKG_RPATH)

server: server.c
	cc server.c -o server $(PKG_CFLAGS) -g -Wl,-rpath=$(PKG_RPATH)

dcuz: $(CPP_FILES) $(HPP_FILES)
	g++ -shared -fPIC -ldl $(CPP_FILES) -o dcuz.so

.PHONY: run_cluster
run_cluster: cluster server dcuz
	LD_PRELOAD=$(PWD)/dcuz.so ./cluster

.PHONY: strace_cluster
strace_cluster: cluster server dcuz
	LD_PRELOAD=$(PWD)/dcuz.so strace -o run.strace -ff ./cluster