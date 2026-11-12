#include "gxps.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted = 0;

static void interrupt_handler(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}

void gx_install_signals(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = interrupt_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
}

bool gx_interrupted(void) { return interrupted != 0; }

double gx_now(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value)) return 0;
    return value.tv_sec + value.tv_nsec / 1e9;
}

double gx_cpu_time(void)
{
    struct rusage value;
    if (getrusage(RUSAGE_SELF, &value)) return 0;
    return value.ru_utime.tv_sec + value.ru_utime.tv_usec / 1e6 +
           value.ru_stime.tv_sec + value.ru_stime.tv_usec / 1e6;
}

uint64_t gx_peak_rss(void)
{
    struct rusage value;
    if (getrusage(RUSAGE_SELF, &value)) return 0;
#ifdef __APPLE__
    return (uint64_t)value.ru_maxrss;
#else
    return (uint64_t)value.ru_maxrss * 1024;
#endif
}

int gx_lock_device(const char *backend, unsigned index, char *error, size_t size)
{
    char path[256];
    int length = snprintf(path, sizeof(path), "/tmp/graphics-xps-%s-%u-%lu.lock",
                          backend, index, (unsigned long)getuid());
    if (length < 0 || (size_t)length >= sizeof(path)) {
        snprintf(error, size, "Device lock path is too long");
        return -1;
    }
    int descriptor = open(path, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (descriptor < 0) {
        snprintf(error, size, "Cannot open GPU test lock: %s", strerror(errno));
        return -1;
    }
    struct stat metadata;
    if (fstat(descriptor, &metadata) || !S_ISREG(metadata.st_mode) || metadata.st_uid != getuid()) {
        snprintf(error, size, "Invalid GPU test lock file");
        close(descriptor);
        return -1;
    }
    if (flock(descriptor, LOCK_EX | LOCK_NB)) {
        snprintf(error, size, "Another graphics-xps process for this user/GPU is running");
        close(descriptor);
        return -1;
    }
    return descriptor;
}

void gx_unlock_device(int descriptor)
{
    if (descriptor >= 0) { flock(descriptor, LOCK_UN); close(descriptor); }
    /* Keep the inode: unlinking could let a second process lock another inode. */
}
