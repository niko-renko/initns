#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
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

#include "../common.h"
#include "cgroup.h"

void init_cgroup(void) {
    char cgpath[PATH_MAX];

    if (mount("cgroup", CGROUP_ROOT, "cgroup2", 0, NULL) < 0 && errno != EBUSY)
        die("cgroup");

    snprintf(cgpath, sizeof(cgpath), "%s/%s", CGROUP_ROOT, CGROUP_NAME);
    if (mkdir(cgpath, 0755) == -1 && errno != EEXIST)
        die("mkdir");
}

int new_cgroup(char *name) {
    char cgpath[PATH_MAX];
    snprintf(cgpath, sizeof(cgpath), "%s/%s/%s", CGROUP_ROOT, CGROUP_NAME,
             name);
    if (mkdir(cgpath, 0755) == -1)
        die("mkdir");
    int fd = open(cgpath, O_DIRECTORY);
    if (fd < 0)
        die("cgroup open");
    return fd;
}

static void wait_cgroup_empty(const char *cgpath) {
    char events_path[PATH_MAX];
    snprintf(events_path, sizeof(events_path), "%s/cgroup.events", cgpath);
    int fd = open(events_path, O_RDONLY);
    if (fd < 0)
        die("cgroup.events open");

    for (;;) {
        char buf[256];
        if (lseek(fd, 0, SEEK_SET) < 0)
            die("cgroup.events lseek");
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n < 0)
            die("cgroup.events read");
        buf[n] = '\0';

        char *p = strstr(buf, "populated ");
        if (!p)
            die("cgroup.events parse");
        if (p[strlen("populated ")] == '0')
            break;

        struct pollfd pfd = {.fd = fd, .events = POLLPRI};
        if (poll(&pfd, 1, -1) < 0 && errno != EINTR)
            die("poll cgroup.events");
    }
    close(fd);
}

static void rm_cgroup_children(char *path) {
    DIR *dir = opendir(path);
    if (!dir)
        die("opendir");

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);

        struct stat st;
        if (lstat(child, &st) == -1)
            die("lstat");

        if (S_ISDIR(st.st_mode)) {
            rm_cgroup_children(child);
            if (rmdir(child) == -1)
                die("rmdir");
        }
    }

    closedir(dir);
}

void rm_cgroup(char *name) {
    char cgpath[PATH_MAX];
    snprintf(cgpath, sizeof(cgpath), "%s/%s/%s", CGROUP_ROOT, CGROUP_NAME,
             name);
    wait_cgroup_empty(cgpath);
    rm_cgroup_children(cgpath);
    if (rmdir(cgpath) == -1)
        die("rmdir");
}

void set_frozen_cgroup(char *name, int frozen) {
    char freeze_path[PATH_MAX];
    char *frozen_str = frozen ? "1" : "0";
    snprintf(freeze_path, sizeof(freeze_path), "%s/%s/%s/cgroup.freeze",
             CGROUP_ROOT, CGROUP_NAME, name);
    int fd = open(freeze_path, O_WRONLY);
    if (fd < 0)
        die("cgroup.freeze open");
    if (write(fd, frozen_str, 1) != 1)
        die("freeze write");
    close(fd);
}

void kill_cgroup(char *name) {
    char kill_path[PATH_MAX];
    snprintf(kill_path, sizeof(kill_path), "%s/%s/%s/cgroup.kill", CGROUP_ROOT,
             CGROUP_NAME, name);
    int fd = open(kill_path, O_WRONLY);
    if (fd < 0)
        die("cgroup.kill open");
    if (write(fd, "1", 1) != 1)
        die("kill write");
    close(fd);
}
