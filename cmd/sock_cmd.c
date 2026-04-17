#include <errno.h>
#include <fcntl.h>
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
#include "../state/state.h"

void cmd(int, int);

static void *sock_cmd(void *arg) {
    State *state = arg;
    set_state(state);

    int fd, cfd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1)
        die("socket");

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    unlink(addr.sun_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
        die("bind");

    if (listen(fd, 5) == -1)
        die("listen");

    for (;;) {
        cfd = accept(fd, NULL, NULL);

        if (cfd == -1) {
            if (errno == EINTR)
                continue;
            die("accept");
        }

        cmd(cfd, cfd);
        close(cfd);
    }
    return NULL;
}

void spawn_sock_cmd(void) {
    pthread_t sock_cmd_t;
    if (pthread_create(&sock_cmd_t, NULL, sock_cmd, get_state()) != 0)
        die("pthread_create");
}