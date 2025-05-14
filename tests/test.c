#include <unistd.h>
#include <string.h>

int main(int argc, char *argv[]) {
    write(0, "Testing\n", 9);

    if (argc == 1) {
        // Test execve
        int pid = fork();
        if (pid) {
            char *argv[] = {"./test", "0", NULL};
            char *envp[] = {NULL};
            execve("./test", argv, envp);
        }

        // Test read/write
        int fds[2];
        pipe(fds);
        pid = fork();

        char buf[100];
        if (pid) {
            read(fds[0], buf, 100);
            write(0, buf, strlen(buf));
        } else {
            write(fds[1], "Hi\n", 4);
        }
    }
    return 0;
}