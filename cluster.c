#define _XOPEN_SOURCE 500

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <ftw.h>

#define N_SERVERS 3 /* Number of servers in the example cluster */

int unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    int rv = remove(fpath);

    if (rv)
        perror(fpath);

    return rv;
}

int rmrf(const char *path)
{
    return nftw(path, unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
}

static int clearDir(const char *dir)
{
    int rv;
    struct stat sb;
    rv = stat(dir, &sb);

    if (rv == 0) {
        rv = rmrf(dir);
        if (rv != 0) {
            printf("error: rmrf directory '%s': %d\n", dir, rv);
            return -1;
        }
    } else if (rv == -1 && errno != ENOENT) {
        printf("error: stat directory '%s': %s\n", dir, strerror(errno));
        return -1;
    }

    // Now, dir does not exist.
    rv = mkdir(dir, 0700);
    if (rv != 0) {
        printf("error: create directory '%s': %s\n", dir, strerror(errno));
        return -1;
    }

    return 0;
}
static void forkServer(const char *topLevelDir, unsigned i, pid_t *pid)
{
    *pid = fork();
    if (*pid == 0) {
        char *dir = malloc(strlen(topLevelDir) + strlen("/D") + 1);
        char *id = malloc(N_SERVERS / 10 + 2);
        char *argv[] = {"./server", dir, id, NULL};
        char *envp[] = {NULL};
        int rv;
        sprintf(dir, "%s/%u", topLevelDir, i + 1);
        rv = clearDir(dir);
        if (rv != 0) {
            abort();
        }
        sprintf(id, "%u", i + 1);
        execve("./server", argv, envp);
    }
}

int main(int argc, char *argv[])
{
    const char *topLevelDir = "/tmp/raft";
    pid_t pids[N_SERVERS];
    unsigned i;
    int rv;

    if (argc > 2) {
        printf("usage: example-cluster [<dir>]\n");
        return 1;
    }

    if (argc == 2) {
        topLevelDir = argv[1];
    }

    /* Make sure the top level directory exists. */
    rv = clearDir(topLevelDir);
    if (rv != 0) {
        return rv;
    }

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* Spawn the cluster nodes */
    for (i = 0; i < N_SERVERS; i++) {
        forkServer(topLevelDir, i, &pids[i]);
        usleep(1000);
    }

    // Wait for servers to finish
    waitpid(-1, NULL, 0);
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);

    // Once one child exits, we can terminate the others.
    for (i = 0; i < N_SERVERS; i++) {
        kill(pids[i], SIGINT);
    }

    long billion = 1000000000L;
    long ns_passed = billion * (end.tv_sec - start.tv_sec) + (long)(end.tv_nsec) - (long)(start.tv_nsec);

    printf("%ld\n", ns_passed);
    return 0;
}
