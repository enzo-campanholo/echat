/* Fail one append's sync after its complete log record has been written. */
#undef fsync
#include <errno.h>
#include <stdlib.h>
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
