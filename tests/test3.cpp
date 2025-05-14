#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>       // For fork, read, write, close, usleep, getpid
#include <sys/socket.h>   // For socket, bind, listen, accept, connect
#include <sys/wait.h>     // For waitpid
#include <sys/epoll.h>    // For epoll_create1, epoll_ctl, epoll_wait
#include <netinet/in.h>   // For sockaddr_in, INADDR_LOOPBACK, htons
#include <arpa/inet.h>    // For inet_ntop (debugging, not strictly needed here)
#include <cstdio>         // For perror, snprintf
#include <cstdlib>        // For exit, EXIT_FAILURE, EXIT_SUCCESS
#include <cstring>        // For strlen, memset
#include <cerrno>         // For errno, strerror
#include <map>            // To map fd to child ID for logging

// Define ports for the two server sockets
const uint16_t PORT_BASE = 12345;
const int MAX_EPOLL_EVENTS = 10; // Max events to handle in one epoll_wait call

/**
 * @brief Sets up a listening socket on the given port.
 *
 * @param port The port number to listen on.
 * @return The file descriptor of the listening socket, or -1 on error.
 */
int setup_listening_socket(uint16_t port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket() failed for listening socket");
        return -1;
    }

    // Allow address reuse
    int optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        close(listen_fd);
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // Listen on 127.0.0.1
    serv_addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        fprintf(stderr, "bind() failed for port %u: %s\n", port, strerror(errno));
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 20) < 0) { // Listen with a backlog of 5
        perror("listen() failed");
        close(listen_fd);
        return -1;
    }
    printf("[Parent Setup] Listening socket fd %d created for port %u\n", listen_fd, port);
    return listen_fd;
}

/**
 * @brief Child process connects to the server and writes data.
 *
 * @param child_id Identifier for the child.
 * @param target_port Port to connect to.
 */
void child_client_writer(int child_id, uint16_t target_port) {
    printf("[Child %d, PID %d] Attempting to connect to 127.0.0.1:%u\n", child_id, getpid(), target_port);

    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket() failed in child");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(target_port);
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        perror("inet_pton failed in child");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    // Retry connect slightly in case server isn't quite ready (simple retry loop)
    int connect_attempts = 0;
    while (connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        connect_attempts++;
        if (errno == ECONNREFUSED && connect_attempts < 5) {
             fprintf(stderr, "[Child %d] connect() to port %u refused, retrying (%d/5)...\n", child_id, target_port, connect_attempts);
             usleep(100000); // Wait 100ms before retry
        } else {
            fprintf(stderr, "[Child %d] connect() to port %u failed: %s\n", child_id, target_port, strerror(errno));
            close(sock_fd);
            exit(EXIT_FAILURE);
        }
    }


    printf("[Child %d, PID %d] Connected to port %u using fd %d. Starting to write.\n", child_id, getpid(), target_port, sock_fd);

	size_t niters = 100;
    for (int i = 1; i <= niters; ++i) {
        char buffer[100];
        int len = snprintf(buffer, sizeof(buffer), "Child %d (via port %u): %d\n", child_id, target_port, i);
        if (len < 0 || len >= (int)sizeof(buffer)) {
            fprintf(stderr, "[Child %d] snprintf error\n", child_id);
            continue;
        }

        ssize_t bytes_written = write(sock_fd, buffer, len);
        if (bytes_written < 0) {
            fprintf(stderr, "[Child %d] Write error to fd %d: %s\n", child_id, sock_fd, strerror(errno));
            break;
        } else if (bytes_written != len) {
            fprintf(stderr, "[Child %d] Partial write (%zd / %d bytes)\n", child_id, bytes_written, len);
        }
        usleep(10000 + (child_id * 1000)); // Small delay
    }

    printf("[Child %d] Finished writing. Closing fd %d.\n", child_id, sock_fd);
    if (close(sock_fd) == -1) {
       fprintf(stderr, "[Child %d] Error closing connected_fd %d: %s\n", child_id, sock_fd, strerror(errno));
    }
    printf("[Child %d] Exiting.\n", child_id);
    exit(EXIT_SUCCESS);
}


int main() {
	constexpr size_t nchildren = 10;
	int listen_fds[nchildren];
	int conn_fds[nchildren];
	int pids[nchildren];
    int epoll_fd = -1; // epoll instance file descriptor

    // Setup first listening socket
	for (int i = 0; i < nchildren; i++) {
		listen_fds[i] = setup_listening_socket(PORT_BASE + i);
	}

    // Create epoll instance
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        perror("[Parent] epoll_create1 failed");
        return EXIT_FAILURE;
    }
    printf("[Parent] Created epoll instance with fd %d\n", epoll_fd);

	// Fork children
	for (int i = 0; i < nchildren; i++) {
		pids[i] = fork();
		if (pids[i] == 0) {
			for (int j = 0; j < nchildren; j++) close(listen_fds[j]);
			close(epoll_fd);
			child_client_writer(i+1, PORT_BASE + i);
		}
		printf("[Parent] Forked child %d (PID %d) to connect to port %u.\n", i+1, pids[i], PORT_BASE + i);
	}

	for (int i = 0; i < nchildren; i++) {
		printf("[Parent] Waiting to accept connection on port %u (from child %d)...\n", PORT_BASE + i, i+1);
		struct sockaddr_in client_addr;
    	socklen_t client_len = sizeof(client_addr);
    	conn_fds[i] = accept(listen_fds[i], (struct sockaddr*) &client_addr, &client_len);
    	if (conn_fds[i] < 0) {
        	perror("[Parent] accept() failed for child 1");
        	return EXIT_FAILURE;
    	}
    	printf("[Parent] Accepted connection from child %d on fd %d. New connection fd: %d.\n", i+1, listen_fds[i], conn_fds[i]);
    	close(listen_fds[i]); // Close listening socket, no longer needed for this child
    	listen_fds[i] = -1;   // Mark as closed

		struct epoll_event ev;
    	ev.events = EPOLLIN | EPOLLET; // Monitor for input, use Edge Triggered mode
    	ev.data.fd = conn_fds[i];
    	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fds[i], &ev) == -1) {
        	perror("[Parent] epoll_ctl: failed to add conn_fd");
        	return EXIT_FAILURE;
    	}
    	printf("[Parent] Added conn_fd=%d to epoll.\n", conn_fds[i]);
	}

    // Parent reads data from accepted connections using epoll
    printf("[Parent] Reading from epoll...\n");

    struct epoll_event events[MAX_EPOLL_EVENTS];
    int open_connections = nchildren;
    std::map<int, int> fd_to_child_id; // Map connection fd to child id for logging
	for (int i = 0; i < nchildren; i++) fd_to_child_id[conn_fds[i]] = i+1;

    while (open_connections > 0) {
        // Wait for events, timeout of 5 seconds (5000 ms)
        int num_events = epoll_pwait(epoll_fd, events, MAX_EPOLL_EVENTS, 5000, 0);

        if (num_events < 0) {
            if (errno == EINTR) { // Interrupted by signal, retry
                continue;
            }
            perror("[Parent] epoll_wait failed");
            break; // Exit loop on error
        }
        if (num_events == 0) {
            printf("[Parent] epoll_wait timed out.\n");
            continue; // No events, continue waiting
        }

        // Process reported events
        for (int i = 0; i < num_events; ++i) {
            int current_fd = events[i].data.fd;
            int child_id = fd_to_child_id.count(current_fd) ? fd_to_child_id[current_fd] : 0; // Get child ID for logging

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                // Error or hangup on the socket
                fprintf(stderr, "[Parent] EPOLLERR/EPOLLHUP on fd %d (Child %d). Closing connection.\n", current_fd, child_id);
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, current_fd, NULL); // Remove from epoll
                close(current_fd);
                open_connections--;
                fd_to_child_id.erase(current_fd); // Remove from map
            } else if (events[i].events & EPOLLIN) {
                // Data available to read
                // Since we use Edge Triggered (EPOLLET), we must read until EAGAIN/EWOULDBLOCK
                while (true) {
                    char read_buffer[256];
                    ssize_t bytes_read = read(current_fd, read_buffer, sizeof(read_buffer) - 1);

                    if (bytes_read < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // No more data to read for now (ET mode)
                            // printf("[Parent] Read on fd %d (Child %d) returned EAGAIN/EWOULDBLOCK.\n", current_fd, child_id);
                            break; // Exit inner read loop
                        } else {
                            // Actual read error
                            fprintf(stderr, "[Parent] Read error on fd %d (Child %d): %s. Closing connection.\n", current_fd, child_id, strerror(errno));
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, current_fd, NULL);
                            close(current_fd);
                            open_connections--;
                             fd_to_child_id.erase(current_fd);
                            break; // Exit inner read loop
                        }
                    } else if (bytes_read == 0) {
                        // EOF - Client disconnected gracefully
                        printf("[Parent] Read returned 0 (EOF) on fd %d. Child %d disconnected.\n", current_fd, child_id);
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, current_fd, NULL);
                        close(current_fd);
                        open_connections--;
                         fd_to_child_id.erase(current_fd);
                        break; // Exit inner read loop
                    } else {
                        // Successfully read data
                        read_buffer[bytes_read] = '\0'; // Null-terminate
                        printf("[Parent] Received %zd bytes on fd %d (from Child %d): %s", bytes_read, current_fd, child_id, read_buffer);
                        if (read_buffer[bytes_read - 1] != '\n') {
                            printf("\n"); // Add newline if not present
                        }
                    }
                } // End inner read loop (ET handling)
            } // End EPOLLIN handling
        } // End for loop processing events
    } // End while open_connections > 0

    printf("[Parent] Finished reading from children (open_connections = %d).\n", open_connections);

    // Wait for children
    printf("[Parent] Waiting for children to exit...\n");
    int status;
	for (int i = 0; i < nchildren; i++) {
		waitpid(pids[i], &status, 0);
		printf("[Parent] Child %d (PID %d) exited with status %d.\n", i+1, pids[i], WEXITSTATUS(status));
	}

    printf("[Parent] Exiting.\n");
    return EXIT_SUCCESS;
}
