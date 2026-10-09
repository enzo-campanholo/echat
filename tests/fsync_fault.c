/* Inject storage faults without changing production code or using a loader. */
#undef fsync
#undef fopen
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int unii_test_fsync(int fd)
{
    static int failed;
    const char *path;
    struct stat file, target;
    path = getenv("UNII_TEST_FSYNC_PATH");
    if (!failed && path && fstat(fd, &file) == 0 && stat(path, &target) == 0 &&
        file.st_dev == target.st_dev && file.st_ino == target.st_ino) {
        failed = 1;
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

FILE *unii_test_fopen(const char *path, const char *mode)
{
    static unsigned int reads;
    const char *target;
    target = getenv("UNII_TEST_READ_PATH");
    if (target && !strcmp(path, target) && !strcmp(mode, "rb") && ++reads == 2) {
        errno = EIO;
        return NULL;
    }
    return fopen(path, mode);
}
