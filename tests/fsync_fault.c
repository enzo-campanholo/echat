/* Inject storage faults without changing production code or using a loader. */
#undef fsync
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned long syncs;

static int is_target(int fd, const char *variable)
{
    const char *path;
    struct stat file, target;
    path = getenv(variable);
    return path && fstat(fd, &file) == 0 && stat(path, &target) == 0 &&
           file.st_dev == target.st_dev && file.st_ino == target.st_ino;
}

/* UNII_TEST_SYNC_COUNT names a file that receives the number of fsyncs. */
static void report(void)
{
    FILE *f;
    f = fopen(getenv("UNII_TEST_SYNC_COUNT"), "w");
    if (f) {
        fprintf(f, "%lu\n", syncs);
        fclose(f);
    }
}

int unii_test_fsync(int fd)
{
    static int failed;
    if (!syncs++ && getenv("UNII_TEST_SYNC_COUNT"))
        atexit(report);
    if (!failed && is_target(fd, "UNII_TEST_FSYNC_PATH")) {
        failed = 1;
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

