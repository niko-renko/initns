#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/input.h>
#include <linux/kd.h>
#include <linux/sched.h>
#include <linux/vt.h>

#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

void die(const char *msg) {
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) {
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "<3>initns: %s: %s\n", msg,
                         strerror(errno));
        if (n > 0)
            write(fd, buf, n);
        close(fd);
    }
    exit(1);
}

void clean_fds(void) {
    for (int fd = 0; fd < sysconf(_SC_OPEN_MAX); fd++)
        close(fd);
}
